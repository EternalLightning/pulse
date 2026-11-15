// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

int RunContentContractCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto left = base / L"left", right = base / L"right";
    std::filesystem::create_directories(left); std::filesystem::create_directories(right);
    Check(Write(left / L"a.txt", "contract_marker") && Write(right / L"b.txt", "contract_marker"), "create dual root contract fixtures");
    ContentSearchRequest source; source.root = left.wstring(); source.roots = {left.wstring(), right.wstring()}; source.needle = L"contract_marker";
    ContentSearchRequest request;
    Check(content::ParseSearchRequest(content::SerializeSearchRequest(source), request) && request.roots == source.roots,
        "production wire round trip preserves disjoint roots");
    ContentIndex db((base / L"scope.sqlite").wstring());
    ContentIndexConfig config; config.roots = {{left.wstring()}, {right.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "index dual root contract fixtures");
    db.Pause(true);
    Check(Search(db, request).size() == 2, "cached search honors both serialized roots");
    Write(right / L"new.txt", "contract_marker");
    std::atomic<bool> cancel{false}; size_t hits = 0; ContentSearchProgress final;
    const bool ok = db.SearchTask(request, cancel, [&](const auto& p, auto batch) { final = p; hits += batch.size(); return true; });
    Check(ok && !final.error && hits == 3, "task cache plus scan honors both serialized roots");
    source.roots.clear(); source.root = right.wstring();
    request = {};
    Check(content::ParseSearchRequest(content::SerializeSearchRequest(source), request) && Search(db, request).size() == 1,
        "legacy single root wire request remains scoped");
    return failed ? 1 : 0;
}

int RunContentRootErrorCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto root = base / L"files", blocked = root / L"blocked";
    std::filesystem::create_directories(blocked);
    Check(Write(root / L"a.txt", "root_marker") && Write(blocked / L"b.txt", "root_marker"), "create incomplete root fixtures");
    std::atomic<bool> fail{true};
    ContentIndexScanHooks hooks;
    hooks.enumeration.first = [&](const auto& path, auto& data) {
        if (fail && ContentScopeKey(path) == ContentScopeKey(blocked.wstring())) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        return FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    };
    ContentIndex db((base / L"errors.sqlite").wstring(), ContentAgentMode::LegacyWriter, hooks);
    ContentIndexConfig config; config.roots = {{root.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "injected incomplete root finishes bounded crawl");
    auto status = db.Status();
    Check(status.root_status.size() == 1 && status.root_status[0].available &&
        status.root_status[0].error == ERROR_ACCESS_DENIED && status.root_status[0].state == ContentIndexRootStatus::State::AccessFailed,
        "accessible partial root exposes per root error and never Ready");
    ContentSearchRequest request; request.needle = L"root_marker";
    Check(Search(db, request).size() == 1, "healthy partial root hit remains searchable");
    fail = false;
    config.excluded_directories.push_back(L"unused-recovery");
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "recovered root rescans");
    status = db.Status();
    Check(status.root_status.size() == 1 && !status.root_status[0].error && status.root_status[0].state == ContentIndexRootStatus::State::Ready && Search(db, request).size() == 2,
        "recovered complete root returns Ready with both hits");
    return failed ? 1 : 0;
}

int RunContentConfigErrorCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto root = base / L"files";
    std::filesystem::create_directories(root);
    Check(Write(root / L"a.txt", "config_marker"), "create explicit database configuration fixture");
    const auto database = (base / L"config.sqlite").wstring();
    {
        ContentIndex db(database); ContentIndexConfig config; config.roots = {{root.wstring()}};
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "seed explicit database and durable sidecar");
    }
    auto bytes = [](const std::wstring& path) {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto before = bytes(database);
    Check(Write(database + L".config", "broken"), "corrupt fixture sidecar without touching cache");
    {
        ContentIndex instant(database, ContentAgentMode::Instant);
        Check(instant.Status().error == ERROR_INVALID_DATA && instant.Configuration().roots.empty(),
            "instant constructor refuses damaged sidecar instead of reviving SQLite roots");
    }
    {
        ContentIndex writer(database);
        const auto deadline = GetTickCount64() + 5000;
        while (!writer.Status().error && GetTickCount64() < deadline) Sleep(5);
        Check(writer.Status().error == ERROR_INVALID_DATA && writer.Configuration().roots.empty(),
            "writer stops on damaged configuration before default-root reconciliation");
    }
    Check(bytes(database) == before && bytes(database + L".config") == "broken",
        "failed constructor leaves existing database and bad sidecar bytes unchanged");
    return failed ? 1 : 0;
}


bool AdvanceLockedContentVersion(HANDLE locked, const std::filesystem::path& file) {
    BY_HANDLE_FILE_INFORMATION before{};
    if (!GetFileInformationByHandle(locked, &before)) return false;
    ULARGE_INTEGER changed{};
    changed.LowPart = before.ftLastWriteTime.dwLowDateTime;
    changed.HighPart = before.ftLastWriteTime.dwHighDateTime;
    changed.QuadPart += 10000000;
    FILETIME time{changed.LowPart, changed.HighPart};
    if (!SetFileTime(locked, nullptr, nullptr, &time)) return false;
    WIN32_FILE_ATTRIBUTE_DATA after{};
    return GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &after) &&
        CompareFileTime(&after.ftLastWriteTime, &before.ftLastWriteTime) != 0;
}

int RunContentRetryRetentionCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto root = base / L"files", database = base / L"retry.sqlite";
    std::filesystem::create_directories(root);
    const auto file = root / L"locked.txt";
    Check(Write(file, "retry_retained_marker"), "seed old cached body for locked-file rescan");
    std::atomic<size_t> scans{0};
    ContentIndexScanHooks hooks;
    hooks.enumeration.first = [&](const auto& path, auto& data) {
        if (ContentScopeKey(path) == ContentScopeKey(root.wstring())) ++scans;
        return FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    };
    ContentIndex db(database.wstring(), ContentAgentMode::LegacyWriter, hooks);
    ContentIndexConfig config; config.roots = {{root.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "locked-file cache initialized");
    HANDLE locked = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "lock already cached file without deleting it");
    if (locked == INVALID_HANDLE_VALUE) return 1;
    struct Unlock { HANDLE h; ~Unlock() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } unlock{locked};
    Check(AdvanceLockedContentVersion(locked, file), "change cached metadata while the old file remains exclusively locked");
    config.excluded_directories.push_back(L"unused-retry-rescan");
    Check(db.Configure(config), "force a complete scan while old file cannot be read");
    const auto deadline = GetTickCount64() + 10000;
    bool completed = false;
    while (GetTickCount64() < deadline) {
        const auto state = db.Status();
        if (scans >= 2 && !state.indexing && state.pending_files) { completed = true; break; }
        Sleep(10);
    }
    ContentSearchRequest request; request.needle = L"retry_retained_marker";
    const auto retained = Search(db, request).size();
    if (!completed || retained != 1) {
        const auto state = db.Status();
        std::fprintf(stderr, "[DIAG] local retry: scans=%zu completed=%d hits=%zu indexing=%d pending=%llu error=%lu\n",
            scans.load(), completed, retained, state.indexing, state.pending_files, state.error);
    }
    Check(completed && retained == 1, "completed initial scan retains discovered body pending sharing retry");
    CloseHandle(unlock.h); unlock.h = INVALID_HANDLE_VALUE;
    Check(db.WaitUntilIdle(10000) && Search(db, request).size() == 1, "retry after unlock retains searchable document");
    std::filesystem::remove(file);
    Check(AwaitHit(db, request, 0), "real deletion still removes retained document");
    return failed ? 1 : 0;
}

int RunContentSystemCacheCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto root = base / L"files", database = base / L"system.sqlite";
    std::filesystem::create_directories(root);
    const auto first = root / L"a.txt", second = root / L"b.txt";
    Check(Write(first, "system_cache_marker") && Write(second, "system_cache_marker"), "seed two isolated cached bodies");
    ContentIndex db(database.wstring(), ContentAgentMode::LegacyWriter);
    ContentIndexConfig config; config.roots = {{root.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "index isolated cache before changing only its stored paths");
    db.Pause(true);
    // Only SQLite rows acquire these synthetic paths; no system file is opened or written.
    const std::wstring system = LR"(C:\Windows\pulse-system-cache.txt)", ordinary = LR"(C:\WindowsOld\pulse-system-cache.txt)";
    sqlite3* connection = nullptr;
    bool seeded = sqlite3_open16(database.c_str(), &connection) == SQLITE_OK;
    if (seeded) {
        sqlite3_busy_timeout(connection, 5000);
        for (const auto& pair : std::vector<std::pair<std::wstring, std::wstring>>{
            {first.wstring(), system}, {second.wstring(), ordinary}}) {
            sqlite3_stmt* update = nullptr;
            const bool prepared = sqlite3_prepare_v2(connection, "UPDATE documents SET path=?2 WHERE path=?1", -1, &update, nullptr) == SQLITE_OK;
            if (prepared) {
                sqlite3_bind_text16(update, 1, pair.first.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text16(update, 2, pair.second.c_str(), -1, SQLITE_TRANSIENT);
            }
            seeded = prepared && sqlite3_step(update) == SQLITE_DONE && sqlite3_changes(connection) == 1 && seeded;
            sqlite3_finalize(update);
        }
    }
    if (!seeded) std::fprintf(stderr, "[DIAG] synthetic cache metadata: sqlite=%d %s\n",
        connection ? sqlite3_errcode(connection) : SQLITE_CANTOPEN, connection ? sqlite3_errmsg(connection) : "open failed");
    if (connection) sqlite3_close(connection);
    Check(seeded, "store synthetic system and sibling paths without filesystem access");
    if (!seeded) return 1;
    ContentSearchRequest request; request.needle = L"system_cache_marker";
    Check(Search(db, request).size() == 2, "explicitly included system location remains searchable from cache");
    request.skip_system_locations = true;
    auto hits = Search(db, request);
    Check(hits.size() == 1 && hits[0].path == ordinary, "cached publication excludes system location and admits WindowsOld");
    request.maximum_hits = 1;
    hits = Search(db, request);
    Check(hits.size() == 1 && hits[0].path == ordinary, "filtered cached system row does not consume the result cap");
    bool scopes = true;
    for (const wchar_t* path : {LR"(C:\Windows\file.txt)", LR"(d:/WINDOWS/file.txt)", LR"(C:\$Recycle.Bin\file.txt)",
            LR"(C:\System Volume Information\file.txt)", LR"(C:\pagefile.sys)", LR"(C:\hiberfil.sys)", LR"(C:\swapfile.sys)"}) {
        scopes &= !ContentRequestIncludesPath(request, path);
        request.skip_system_locations = false;
        scopes &= ContentRequestIncludesPath(request, path);
        request.skip_system_locations = true;
    }
    Check(scopes && ContentRequestIncludesPath(request, ordinary), "live candidate scope uses the same system policy and component boundaries");
    return failed ? 1 : 0;
}
