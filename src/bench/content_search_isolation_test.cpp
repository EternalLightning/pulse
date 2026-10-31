// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "content_search_fixture.h"
#include "../common/command_line.h"
#include "../index/content_search_client.h"
#include <iostream>

int wmain() {
    using namespace pulse;
    int failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failed;
    };
    bench::ContentSearchFixture fixture;
    if (!fixture.Initialize(L"../bench_data/audit-fixes-20261030", L"content-isolation")) {
        check(false, "initialize private database, default configuration and file locator");
        std::cerr << "[INFO] fixture stage=" << fixture.Stage() << " error=" << GetLastError() << std::endl;
        std::wcerr << L"[INFO] attempted=" << fixture.AttemptedPath() << L" fs_error=" << fixture.FilesystemError() << std::endl;
        return 1;
    }
    std::wcout << L"[INFO] private content fixture: " << fixture.Root() << std::endl;
    const auto wait = [](auto ready, DWORD timeout = 10000) {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) { if (ready()) return true; Sleep(10); }
        return ready();
    };
    const auto first = fixture.Root() / L"first", second = fixture.Root() / L"second";
    std::filesystem::create_directory(first); std::filesystem::create_directory(second);
    std::ofstream(first / L"legacy.txt") << "pulse isolation legacy needle";
    std::ofstream(second / L"instant.txt") << "pulse isolation instant needle";
    index::ContentSearchClient client;
    check(!client.SetDatabasePathForTesting(L"relative.sqlite") && GetLastError() == ERROR_INVALID_PARAMETER,
        "stopped client rejects relative database path");
    check(!client.SetDatabasePathForTesting(fixture.DatabasePath() + L"\"bad") && GetLastError() == ERROR_INVALID_PARAMETER,
        "stopped client rejects quote in database path");
    auto nul_path = fixture.DatabasePath(); nul_path.push_back(L'\0'); nul_path += L"bad";
    check(!client.SetDatabasePathForTesting(nul_path) && GetLastError() == ERROR_INVALID_PARAMETER,
        "stopped client rejects embedded NUL in database path");
    check(client.SetDatabasePathForTesting({}), "empty stopped override restores default selection");
    if (!client.SetDatabasePathForTesting(fixture.DatabasePath())) {
        check(false, "bind client to explicit private database"); return 1;
    }
    uint64_t generation = 0;
    const auto query = [&](const std::wstring& needle, const std::wstring& expected_name) {
        index::ContentSearchRequest request;
        request.generation = ++generation; request.needle = needle;
        client.SearchAsync(request);
        bool done = false; DWORD error = ERROR_SUCCESS; std::vector<index::ContentHit> hits;
        const bool completed = wait([&] {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) {
                error = update.progress.error; done |= update.progress.done;
                hits.insert(hits.end(), update.hits.begin(), update.hits.end());
            }
            return done;
        });
        std::cout << "[INFO] query done=" << done << " error=" << error << " hits=" << hits.size() << std::endl;
        return completed && !error && hits.size() == 1 && hits.front().name == expected_name;
    };
    index::ContentIndexConfig legacy; legacy.roots = {{first.wstring()}};
    client.Start(nullptr, 0, index::ContentAgentMode::LegacyWriter);
    check(!client.SetDatabasePathForTesting(fixture.DatabasePath() + L".other") && GetLastError() == ERROR_BUSY,
        "running client rejects database replacement");
    client.Configure(legacy);
    check(wait([&] { const auto status = client.GetStatus(); return client.ConfigurationReady() &&
        !status.error && status.indexed_files == 1 && !status.indexing && !status.pending_files; }),
        "real LegacyWriter helper indexes only the explicit private database");
    check(fixture.ConfigurationMatches(legacy), "LegacyWriter saves selected private configuration");
    check(query(L"pulse isolation legacy needle", L"legacy.txt"), "LegacyWriter query returns private file through IPC");
    client.Stop();

    index::ContentIndexConfig instant; instant.roots = {{second.wstring()}};
    client.Start(nullptr, 0, index::ContentAgentMode::Instant);
    check(wait([&] { return client.ConfigurationReady() && index::config_storage::SameConfig(client.GetConfig(), legacy); }),
        "database override survives Stop/Start and Instant status reads private configuration");
    client.Configure(instant);
    check(wait([&] { return client.ConfigurationReady() && index::config_storage::SameConfig(client.GetConfig(), instant); }),
        "Instant configure publishes selected private configuration");
    check(fixture.ConfigurationMatches(instant), "Instant configure saves selected private sidecar");
    check(query(L"pulse isolation instant needle", L"instant.txt"),
        "real Instant helper loads selected private configuration for each query");
    client.Stop();
    check(fixture.DefaultConfigurationUnchanged(), "private default configuration sentinel stays unchanged");
    check(fixture.LocatorUnchanged(), "private file locator sentinel stays unchanged");

    const auto executable = std::filesystem::absolute(L"Pulse.Index.exe");
    const auto rejects = [&](const std::wstring& arguments) {
        std::wstring command = QuoteWindowsArgument(executable.wstring()) + L" " + arguments;
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) return false;
        CloseHandle(process.hThread);
        const auto result = WaitForSingleObject(process.hProcess, 5000);
        if (result != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, ERROR_CANCELLED); WaitForSingleObject(process.hProcess, 5000); }
        DWORD exit = 0; const bool read = GetExitCodeProcess(process.hProcess, &exit) != FALSE;
        CloseHandle(process.hProcess);
        return result == WAIT_OBJECT_0 && read && exit == ERROR_INVALID_PARAMETER;
    };
    const auto parent = std::to_wstring(GetCurrentProcessId());
    for (const auto* mode : {L"--content-instant-agent", L"--content-index-agent", L"--content-index-observer"}) {
        const auto prefix = std::wstring(mode) + L" isolation " + parent;
        check(rejects(prefix + L" \"\""), "persistent helper rejects empty explicit database path");
        check(rejects(prefix + L" relative.sqlite"), "persistent helper rejects relative explicit database path");
        check(rejects(prefix + L" " + QuoteWindowsArgument(fixture.DatabasePath()) + L" extra"),
            "persistent helper rejects extra arguments");
        check(rejects(std::wstring(mode) + L" invalid/token " + parent + L" " + QuoteWindowsArgument(fixture.DatabasePath())),
            "persistent helper retains token validation with explicit database");
        check(rejects(std::wstring(mode) + L" isolation 0 " + QuoteWindowsArgument(fixture.DatabasePath())),
            "persistent helper retains parent PID validation with explicit database");
    }
    check(fixture.DefaultConfigurationUnchanged(), "default configuration sentinel survives malformed helper requests");
    check(fixture.LocatorUnchanged(), "file locator sentinel survives malformed helper requests");
    return failed ? 1 : 0;
}
