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
