// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/content_config_storage.h"
#include "../index/content_index.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pulse::index;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks; std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
    if (!ok) ++failures;
}
std::string Bytes(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data/issue-fixes-removable/cache-fixture") /
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    wchar_t unused = L'Z';
    const DWORD drives = GetLogicalDrives();
    while (unused > L'F' && (drives & (1u << (unused - L'A')))) --unused;
    const auto offline = std::wstring(1, unused) + L":\\PulseOfflineFixture";
    const auto facts = ProbePersistentDevice(offline);
    Check(!AllowsPersistentIndex(facts) && !KnownRemovableDevice(facts), "unused drive is unknown/offline without evidence of a removable device");
    const auto database = (root / L"legacy.sqlite").wstring();
    ContentIndexConfig config; config.roots = {{offline}};
    sqlite3* db = nullptr;
    Check(sqlite3_open16(database.c_str(), &db) == SQLITE_OK, "private legacy database opens");
    const std::string sql = "CREATE TABLE roots(path TEXT PRIMARY KEY,encoding INTEGER,available INTEGER);"
        "CREATE TABLE settings(name TEXT PRIMARY KEY,value INTEGER);CREATE TABLE exclusions(value TEXT);"
        "CREATE TABLE documents(id INTEGER PRIMARY KEY,path TEXT UNIQUE,root TEXT,size INTEGER,modified INTEGER,body TEXT,epoch INTEGER,version TEXT);";
    bool seeded = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* statement = nullptr;
    seeded &= sqlite3_prepare_v2(db, "INSERT INTO roots VALUES(?1,0,0)", -1, &statement, nullptr) == SQLITE_OK;
    sqlite3_bind_text16(statement, 1, offline.c_str(), -1, SQLITE_TRANSIENT);
    seeded &= sqlite3_step(statement) == SQLITE_DONE; sqlite3_finalize(statement);
    seeded &= sqlite3_prepare_v2(db, "INSERT INTO documents VALUES(1,?1,?2,6,0,'cached',42,'old')", -1, &statement, nullptr) == SQLITE_OK;
    const auto file = offline + L"\\cached.txt";
    sqlite3_bind_text16(statement, 1, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text16(statement, 2, offline.c_str(), -1, SQLITE_TRANSIENT);
    seeded &= sqlite3_step(statement) == SQLITE_DONE; sqlite3_finalize(statement); sqlite3_close(db); db = nullptr;
    Check(seeded, "private legacy fixture contains offline internal root and existing cached document");
    pulse::ipc::PayloadWriter payload; content::PutConfig(payload, config);
    Check(config_storage::WriteSidecar(database + L".config", 1, payload.data()), "private old sidecar written without altering user configuration");
    const auto original = Bytes(database + L".config");
    ContentIndexConfig loaded;
    Check(LoadContentIndexConfig(database, loaded) && loaded.roots.size() == 1 && loaded.roots[0].path == offline,
        "loading durable configuration retains unknown/offline internal root");
    {
        ContentIndex index(database, ContentAgentMode::LegacyWriter);
        Check(index.WaitUntilIdle(10000), "legacy writer settles while offline root remains paused");
        const auto actual = index.Configuration(); const auto status = index.Status();
        Check(actual.roots.size() == 1 && actual.roots[0].path == offline,
            "legacy writer keeps offline root in its configuration");
        Check(status.root_status.size() == 1 && !status.root_status[0].available && !status.root_status[0].indexing,
            "offline root does not register active persistent scanning");
        Check(!index.Configure(config) && GetLastError() == ERROR_NOT_SUPPORTED,
            "new explicit configuration rejects unprobeable device without changing old configuration");
    }
    Check(sqlite3_open16(database.c_str(), &db) == SQLITE_OK, "private legacy database reopens after writer stops");
    bool retained = sqlite3_prepare_v2(db, "SELECT count(*) FROM documents WHERE id=1 AND body='cached' AND epoch=42", -1, &statement, nullptr) == SQLITE_OK;
    retained &= sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1;
    sqlite3_finalize(statement); sqlite3_close(db);
    Check(retained, "offline internal cached document survives without rewritten epoch or prune");
    Check(Bytes(database + L".config") == original, "durable old sidecar remains byte-identical");
    std::cout << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
