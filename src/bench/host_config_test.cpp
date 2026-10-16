#include "../index/index_config_parse.h"
#include "../index/index_service_reload.h"
#include "../index/index_machine_config_actions.h"
#include "../index/content_config_storage.h"
#include "../common/utf8_file.h"
#include "../../third_party/sqlite/sqlite3.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!ok) ++failures;
}
void ServiceCases() {
    using pulse::index::SubmitServiceReload;
    auto run = [](DWORD error, bool manager_fails, bool service_fails, int expected_closed) {
        int closed = 0, controls = 0;
        DWORD last_error = error;
        const DWORD result = SubmitServiceReload(L"fixture", 128,
            [&] { return manager_fails ? nullptr : reinterpret_cast<SC_HANDLE>(uintptr_t{1}); },
            [&](SC_HANDLE, const wchar_t*) { return service_fails ? nullptr : reinterpret_cast<SC_HANDLE>(uintptr_t{2}); },
            [&](SC_HANDLE, DWORD, SERVICE_STATUS*) { ++controls; return error == ERROR_SUCCESS; },
            [&](SC_HANDLE) { ++closed; last_error = ERROR_INVALID_HANDLE; }, [&] { return last_error; });
        return result == error && closed == expected_closed && controls == (manager_fails || service_fails ? 0 : 1);
    };
    Check(run(ERROR_SUCCESS, false, false, 2), "service reload submission returns success and closes handles");
    Check(run(ERROR_SERVICE_DOES_NOT_EXIST, false, true, 1), "missing service returns exact failure");
    Check(run(ERROR_SERVICE_NOT_ACTIVE, false, false, 2), "stopped service returns exact failure");
    Check(run(ERROR_ACCESS_DENIED, false, false, 2), "rejected control returns exact failure");
    Check(run(ERROR_ACCESS_DENIED, true, false, 0), "SCM access denied returns exact failure");
}
void MachineCases() {
    using namespace pulse::index;
    int saves = 0;
    Check(PrepareMachineConfiguration([](IndexConfig& config) { config.index_path = L"C:\\Default"; return false; },
        [&](const IndexConfig&) { ++saves; return true; }) == ERROR_INVALID_DATA && saves == 0,
        "installation never saves default configuration after load failure");
    Check(PrepareMachineConfiguration([](IndexConfig&) { return true; },
        [&](const IndexConfig&) { ++saves; return false; }) == ERROR_WRITE_FAULT && saves == 1,
        "installation stops when validated configuration cannot be saved");
    Check(PrepareMachineConfiguration([](IndexConfig&) { return true; },
        [&](const IndexConfig&) { ++saves; return true; }) == ERROR_SUCCESS && saves == 2,
        "installation accepts successful configuration preparation");
    Check(LoadMachineIndexDirectory([](IndexConfig& config) { config.index_path = L"C:\\Default"; return false; }).empty(),
        "directory resolution cannot use default path after load failure");
    Check(LoadMachineIndexDirectory([](IndexConfig& config) { config.index_path = L"D:\\Configured"; return true; }) == L"D:\\Configured",
        "directory resolution returns validated configured path");
    IndexConfig config;
    Check(ParseMachineConfig(L"{}", L"C:\\Default", config) && config.index_path == L"C:\\Default" && config.include_fixed_ntfs,
        "legacy missing fields retain defaults");
    Check(ParseMachineConfig(LR"({"generation":18446744073709551615,"excluded_paths":["C:\\u\u4e2d","c:/u\u4e2d/"],"excluded_volume_ids":[" volume "]})", L"C:\\Default", config) &&
        config.generation == UINT64_MAX && config.excluded_paths.size() == 1 && config.excluded_paths.front() == L"C:\\u中" && config.excluded_volume_ids.contains(L"VOLUME"),
        "uint64 generation and Unicode arrays parse with existing path normalization");
    Check(!AdvanceMachineConfigGeneration(config) && config.generation == UINT64_MAX,
        "exhausted generation cannot wrap during configuration mutation");
    config.generation = UINT64_MAX - 1;
    Check(AdvanceMachineConfigGeneration(config) && config.generation == UINT64_MAX,
        "last representable generation remains usable");
    for (const auto* text : {L"{", L"{}tail", L"[]", L"{\"excluded_paths\":false}", L"{\"excluded_paths\":[1]}",
        L"{\"include_fixed_ntfs\":\"false\"}", L"{\"generation\":18446744073709551616}", L"{\"generation\":-1}",
        L"{\"generation\":1.5}", L"{\"version\":4294967296}", L"{\"version\":2}", L"{\"index_path\":3}",
        L"{\"excluded_paths\":[\"a\" \"b\"]}", L"{\"excluded_paths\":[\"\\q\"]}", L"{\"index_path\":\"a\\u0000b\"}",
        L"{\"generation\":1,\"generation\":2}"}) {
        IndexConfig sentinel; sentinel.index_path = L"unchanged";
        Check(!ParseMachineConfig(text, L"C:\\Default", sentinel) && sentinel.index_path == L"unchanged",
            "invalid machine configuration rejected without partial publication");
    }
}
void ContentCases() {
    namespace fs = std::filesystem;
    using namespace pulse::index;
    const auto fixture = fs::absolute(fs::path(L"bench_data") /
        (L"host-config-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(fixture);
    SetEnvironmentVariableW(L"LOCALAPPDATA", fixture.c_str());
    pulse::storage::OverrideDefaultRootForTesting(fixture.wstring());
    const auto database = config_storage::DatabasePath();
    const auto sidecar = config_storage::ConfigurationPath(database);
    ContentIndexConfig config;
    Check(LoadContentIndexConfig(config) && config.roots.empty(), "absent sidecar and database use defaults");
    sqlite3* db = nullptr;
    Check(sqlite3_open16(database.c_str(), &db) == SQLITE_OK, "create isolated legacy database");
    auto sql = [&](const char* command) { return sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK; };
    Check(sql("CREATE TABLE roots(path TEXT,encoding INTEGER);CREATE TABLE settings(name TEXT,value INTEGER);CREATE TABLE exclusions(value TEXT);"
        "INSERT INTO roots VALUES('C:\\Legacy',1);INSERT INTO settings VALUES('maximum_file_bytes',1024);INSERT INTO settings VALUES('maximum_document_bytes',2048);"
        "INSERT INTO exclusions VALUES('oldbuild');"), "seed legacy configuration schema");
    Check(LoadContentIndexConfig(config) && config.roots.size() == 1 && config.maximum_file_bytes == 1024 && config.excluded_directories == std::vector<std::wstring>{L"oldbuild"},
        "valid legacy database loads all configuration");
    const auto before = config;
    Check(sql("DROP TABLE exclusions"), "inject late legacy table failure");
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_INVALID_DATA && config_storage::SameConfig(config, before),
        "late table failure rejects partial legacy configuration");
    Check(sql("CREATE TABLE exclusions(value TEXT);UPDATE settings SET value=-1 WHERE name='maximum_file_bytes'"), "inject invalid legacy numeric setting");
    Check(!LoadContentIndexConfig(config) && config_storage::SameConfig(config, before), "negative SQLite size rejected without unsigned wraparound");
    Check(sql("UPDATE settings SET value=1024 WHERE name='maximum_file_bytes'"), "restore valid legacy settings");
    Check(sql("INSERT INTO roots VALUES('D:\\Later',4294967296)"), "inject invalid encoding after valid root row");
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_INVALID_DATA && config_storage::SameConfig(config, before),
        "late invalid root row rejects previously decoded legacy roots");
    Check(sql("DELETE FROM roots WHERE path='D:\\Later'"), "restore valid legacy roots");
    Check(sql("BEGIN EXCLUSIVE"), "hold isolated exclusive database lock");
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_BUSY && config_storage::SameConfig(config, before),
        "busy SQLite read fails without publishing defaults");
    Check(sql("ROLLBACK"), "release isolated database lock");
    ContentIndexConfig sidecar_config; sidecar_config.roots = {{L"D:\\Sidecar"}};
    Check(config_storage::WriteConfigFile(database, sidecar_config) && LoadContentIndexConfig(config) && config_storage::SameConfig(config, sidecar_config),
        "valid sidecar wins over legacy database");
    pulse::ipc::PayloadWriter truncated; content::PutConfig(truncated, before);
    auto bytes = truncated.data(); bytes.pop_back();
    Check(config_storage::WriteSidecar(sidecar, 1, bytes), "inject late truncated configuration payload");
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_INVALID_DATA && config_storage::SameConfig(config, sidecar_config),
        "partial sidecar decode leaves caller configuration unchanged");
    Check(pulse::WriteUtf8FileAtomic(sidecar, L"broken"), "inject corrupt sidecar");
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_INVALID_DATA && config_storage::SameConfig(config, sidecar_config),
        "corrupt sidecar cannot revive legacy roots");
    ContentIndexConfig actual = sidecar_config;
    Check(!config_storage::LoadOrCreateConfig(database, before, actual) && config_storage::SameConfig(actual, sidecar_config),
        "automatic initialization preserves corrupt sidecar and caller output");
    std::wstring unchanged;
    Check(pulse::ReadUtf8File(sidecar, unchanged) && unchanged == L"broken", "corrupt sidecar bytes preserved for recovery");
    sqlite3_close(db);
    fs::remove(sidecar);
    { std::ofstream corrupt(fs::path(database), std::ios::binary | std::ios::trunc); corrupt << "not a database"; }
    Check(!LoadContentIndexConfig(config) && GetLastError() == ERROR_INVALID_DATA && config_storage::SameConfig(config, sidecar_config),
        "corrupt legacy database rejected without output mutation");
    std::error_code error;
    fs::remove_all(fixture, error);
    Check(!error && !fs::exists(fixture), "clean isolated configuration fixture");
}
}
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    const std::string_view selector = argc > 1 ? argv[1] : "";
    if (selector.empty() || selector == "--service") ServiceCases();
    if (selector.empty() || selector == "--machine") MachineCases();
    if (selector.empty() || selector == "--content") ContentCases();
    std::cout << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
