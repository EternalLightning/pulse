// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/snapshot_patch.h"
#include "../app/link_resolve.h"
#include "../ipc/shell_query_client.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
using namespace pulse;
ipc::ShellQueryResult reply;
int queries = 0;
int failures = 0;
std::wstring queried_path;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
bool Same(const fs::DirEntry& a, const fs::DirEntry& b) {
    return a.name == b.name && a.size == b.size && a.attrs == b.attrs &&
        a.is_dir == b.is_dir && a.is_reparse == b.is_reparse && a.cloud_recall == b.cloud_recall &&
        CompareFileTime(&a.mtime, &b.mtime) == 0 && a.link_target == b.link_target &&
        a.link_target_size == b.link_target_size && a.link_target_is_dir == b.link_target_is_dir &&
        CompareFileTime(&a.link_target_mtime, &b.link_target_mtime) == 0;
}
std::vector<fs::DirEntry> Full(const std::wstring& folder) {
    std::vector<fs::DirEntry> rows;
    if (!fs::EnumerateDirectory(folder, rows)) return {};
    app::ResolveLinksInPlace(folder, rows, {});
    return rows;
}
bool MatchesFull(const std::wstring& folder, const std::vector<fs::DirEntry>& rows) {
    const auto full = Full(folder);
    if (rows.size() != full.size()) return false;
    for (const auto& row : rows) {
        const auto found = std::find_if(full.begin(), full.end(), [&](const auto& entry) {
            return entry.name == row.name;
        });
        if (found == full.end() || !Same(row, *found)) return false;
    }
    return true;
}
bool Apply(const std::wstring& folder, std::vector<fs::DirEntry>& rows,
           DWORD action, const wchar_t* name, const wchar_t* old_name = L"") {
    const auto resolved = app::ResolveDirNotifies(folder, {{action, name, old_name}});
    return app::ApplyResolvedDirNotifies(rows, resolved) == app::NotifyPatch::Applied;
}
}

namespace pulse::ipc {
bool QueryShell(const ShellQueryRequest& request, ShellQueryResult& result,
                const std::function<bool()>&, DWORD) {
    ++queries;
    queried_path = request.path;
    Check(request.kind == ShellQueryKind::Shortcut, "incremental resolver uses existing shortcut query");
    result = reply;
    return true;
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace pulse;
    const auto parent = std::filesystem::absolute(L"bench_data/audit-fixes-20261030-batch2");
    const auto folder = parent / (L"snapshot-fixture-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(parent);
    if (!std::filesystem::create_directory(folder)) return 2;
    struct Cleanup {
        std::filesystem::path folder;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(folder, error); }
    } cleanup{folder};
    const auto write = [&](const wchar_t* name, const char* data) {
        std::ofstream file(folder / name, std::ios::binary | std::ios::trunc);
        file << data;
        file.close();
        return !file.fail();
    };
    std::vector<fs::DirEntry> rows;
    Check(write(L"plain.txt", "abc"), "create isolated ordinary file");
    Check(Apply(folder.wstring(), rows, FILE_ACTION_ADDED, L"plain.txt") && queries == 0 &&
        rows.size() == 1 && rows[0].size == 3 && rows[0].link_target.empty(),
        "ordinary notification preserves file metadata without Shell query");
    Check(MatchesFull(folder.wstring(), rows), "ordinary notification matches full enumeration");

    Check(write(L"folder.lnk", "controlled Shell IPC fixture"), "create isolated shortcut file");
    reply.error = S_OK;
    reply.text = (folder / L"target-a").wstring();
    reply.target_is_dir = true;
    reply.target_mtime = {31, 37};
    Check(Apply(folder.wstring(), rows, FILE_ACTION_ADDED, L"folder.lnk") &&
        queried_path == (folder / L"folder.lnk").wstring() && rows.size() == 2 &&
        rows.back().link_target == fs::NormalizePath(reply.text) && rows.back().link_target_is_dir,
        "added directory shortcut resolves target before snapshot application");
    Check(MatchesFull(folder.wstring(), rows), "added shortcut matches full enumeration");

    reply.text = (folder / L"target-b.txt").wstring();
    reply.target_is_dir = false;
    reply.target_size = 987;
    reply.target_mtime = {41, 43};
    Check(Apply(folder.wstring(), rows, FILE_ACTION_MODIFIED, L"folder.lnk") &&
        rows.back().link_target == fs::NormalizePath(reply.text) && !rows.back().link_target_is_dir &&
        rows.back().link_target_size == 987 && rows.back().link_target_mtime.dwLowDateTime == 41,
        "modified shortcut replaces all old target metadata with newly resolved facts");
    Check(MatchesFull(folder.wstring(), rows), "modified shortcut matches full enumeration");

    std::filesystem::rename(folder / L"folder.lnk", folder / L"renamed.LNK");
    Check(Apply(folder.wstring(), rows, FILE_ACTION_RENAMED_NEW_NAME, L"renamed.LNK", L"folder.lnk") &&
        rows.size() == 2 && rows.back().name == L"renamed.LNK" &&
        queried_path == (folder / L"renamed.LNK").wstring(),
        "rename resolves new physical path with case insensitive shortcut suffix");
    Check(MatchesFull(folder.wstring(), rows), "renamed shortcut matches full enumeration");

    reply.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    Check(Apply(folder.wstring(), rows, FILE_ACTION_MODIFIED, L"renamed.LNK") &&
        rows.back().link_target.empty() && !rows.back().link_target_is_dir &&
        rows.back().link_target_size == 0 && rows.back().link_target_mtime.dwLowDateTime == 0,
        "unresolved replacement clears stale shortcut target metadata");
    Check(MatchesFull(folder.wstring(), rows), "broken shortcut matches full enumeration");

    const auto before = rows;
    const int before_queries = queries;
    const auto failed = app::ResolveDirNotifies(folder.wstring(), {
        {FILE_ACTION_MODIFIED, L"renamed.LNK", {}}, {FILE_ACTION_ADDED, L"missing.lnk", {}}});
    Check(app::ApplyResolvedDirNotifies(rows, failed) == app::NotifyPatch::NeedFullEnum &&
        queries == before_queries && rows.size() == before.size() && Same(rows.back(), before.back()),
        "inconclusive stat batch keeps held snapshot and never queries partial facts");
    std::filesystem::remove(folder / L"renamed.LNK");
    Check(Apply(folder.wstring(), rows, FILE_ACTION_REMOVED, L"renamed.LNK") &&
        rows.size() == 1 && queries == before_queries,
        "removed shortcut neither queries removed content nor retains target row");
    std::printf("snapshot shortcut patch: %d failures\n", failures);
    return failures ? 1 : 0;
}
