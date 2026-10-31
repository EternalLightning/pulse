// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../index/user_index_storage.h"
#include "../common/command_line.h"
#include "../index/content_config_storage.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

int ConfigEqualityTest() {
    namespace fs = std::filesystem;
    namespace config_storage = pulse::index::config_storage;
    using pulse::index::ContentIndexConfig;
    const auto fixture = fs::absolute(fs::path(L"bench_data") /
        (L"config-equality-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(fixture);
    SetEnvironmentVariableW(L"LOCALAPPDATA", fixture.c_str());
    pulse::storage::OverrideDefaultRootForTesting(fixture.wstring());
    const auto database = (fixture / L"isolated.sqlite").wstring();
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failed;
    };
    ContentIndexConfig duplicate, distinct;
    duplicate.roots = {{L"C:\\First"}, {L"c:\\first"}};
    distinct = duplicate;
    distinct.roots[1].path = L"C:\\Second";
    check(!config_storage::SameConfig(duplicate, distinct) && !config_storage::SameConfig(distinct, duplicate),
        "duplicate roots cannot conceal another root in either comparison direction");
    auto reordered = distinct;
    std::reverse(reordered.roots.begin(), reordered.roots.end());
    reordered.roots[1].path = L"c:\\FIRST";
    check(config_storage::SameConfig(distinct, reordered), "root order and path case remain insignificant");
    reordered.roots[1].encoding = pulse::text::Encoding::Utf8;
    check(!config_storage::SameConfig(distinct, reordered), "root encoding changes remain significant");
    check(config_storage::WriteConfigFile(database, duplicate), "seed isolated duplicate-root configuration");
    ContentIndexConfig loaded;
    check(config_storage::WriteConfigFile(database, distinct) && config_storage::ReadConfigFile(database, loaded) &&
        loaded.roots.size() == 2 && loaded.roots[1].path == L"C:\\Second",
        "replacing duplicate root persists the newly selected root");
    duplicate = distinct;
    duplicate.excluded_directories = {L"build", L"BUILD"};
    distinct.excluded_directories = {L"build", L"private"};
    check(!config_storage::SameConfig(duplicate, distinct) && !config_storage::SameConfig(distinct, duplicate),
        "duplicate exclusions cannot conceal a newly excluded directory");
    check(config_storage::WriteConfigFile(database, duplicate) && config_storage::WriteConfigFile(database, distinct) &&
        config_storage::ReadConfigFile(database, loaded) && loaded.excluded_directories == distinct.excluded_directories,
        "replacing duplicate exclusion persists the new exclusion");
    reordered = distinct;
    reordered.excluded_directories = {L"PRIVATE", L"Build"};
    check(config_storage::SameConfig(distinct, reordered), "exclusion order and case remain insignificant");
    std::cout << "failures=" << failed << "; isolated fixture retained for inspection\n";
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (argc == 2 && std::string_view(argv[1]) == "--config-equality") return ConfigEqualityTest();
    namespace fs = std::filesystem;
    namespace storage = pulse::storage;
    const auto fixture = fs::absolute(fs::path(L"bench_data") /
        (L"user-index-storage-" + std::to_wstring(GetCurrentProcessId())));
    const auto source = fixture / L"旧目录", target = fixture / L"新索引";
    fs::create_directories(source);
    storage::OverrideDefaultRootForTesting(source.wstring());
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failed;
    };
    auto write = [](const fs::path& path, const char* bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << bytes;
    };
    write(source / L"pulse-index.bin", "checkpoint");
    write(source / L"v9/0123456789abcdef/base-a.bin", "shard");
    write(source / L"app.json", "user preferences");
    write(source / L"notes.txt", "user document");
    std::wstring error;
    check(storage::UserIndexRoot() == source.wstring(), "legacy root unchanged");
    check(storage::Schedule(storage::Kind::Index, target.wstring(), error), "schedule custom index");
    check(storage::UserIndexRoot() == source.wstring(), "schedule preserves current writer directory");
    check(storage::ApplyUserIndexCheckpoint(error), "checkpoint migrated and committed");
    check(storage::UserIndexRoot() == target.wstring() && storage::Pending(storage::Kind::Index).empty(),
        "new index root activated after verified copy");
    check(fs::exists(target / L"pulse-index.bin") && fs::exists(target / L"v9/0123456789abcdef/base-a.bin"),
        "both checkpoint layouts copied");
    check(fs::exists(source / L"pulse-index.bin") && fs::exists(source / L"app.json") &&
        !fs::exists(target / L"app.json") && !fs::exists(target / L"notes.txt"),
        "recovery checkpoint and unrelated source files retained");
    const auto conflict = fixture / L"冲突";
    check(storage::Schedule(storage::Kind::Index, conflict.wstring(), error), "schedule second location");
    write(conflict / L"pulse-index.bin", "conflicting checkpoint");
    check(!storage::ApplyUserIndexCheckpoint(error), "late destination conflict rejects migration");
    check(storage::UserIndexRoot() == target.wstring() && fs::exists(target / L"pulse-index.bin") &&
        !storage::LastError(storage::Kind::Index).empty(), "failure retains active index and exposes error");
    check(storage::Schedule(storage::Kind::Index, target.wstring(), error) &&
        storage::Pending(storage::Kind::Index).empty(), "choose current directory cancels pending migration");
    if (argc > 1) {
        const auto token = L"storage-" + std::to_wstring(GetCurrentProcessId());
        const auto pipe = L"\\\\.\\pipe\\PulseIndex.Test." + token;
        const auto mutex = L"Local\\Pulse.Index.Test." + token;
        const auto host_root = fixture / L"host-checkpoint", host_target = fixture / L"host-migrated";
        const auto directory = fixture / L"files";
        write(directory / L"example.txt", "fixture");
        fs::create_directories(host_root);
        storage::OverrideDefaultRootForTesting(host_root.wstring());
        wchar_t current[32768]{};
        GetModuleFileNameW(nullptr, current, ARRAYSIZE(current));
        const auto exe = fs::path(current).parent_path() / L"Pulse.Index.exe";
        auto command = pulse::QuoteWindowsArgument(exe.wstring()) + L" --test-host " + token + L" " +
            pulse::QuoteWindowsArgument(directory.wstring()) + L" " + pulse::QuoteWindowsArgument(host_root.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        check(started, "owned fixture index host started");
        if (started) {
            bool ready = false;
            for (int attempt = 0; attempt < 100 && !ready; ++attempt) {
                ready = WaitNamedPipeW(pipe.c_str(), 50) != FALSE;
                if (!ready) Sleep(50);
            }
            check(ready, "owned fixture pipe ready");
            bool checkpoint = false;
            for (int attempt = 0; attempt < 100 && !checkpoint; ++attempt) {
                checkpoint = fs::exists(host_root / L"pulse-index.bin");
                if (!checkpoint) Sleep(50);
            }
            check(checkpoint, "owned writer produced a checkpoint");
            check(storage::Schedule(storage::Kind::Index, host_target.wstring(), error), "schedule while fixture writer running");
            check(ready && storage::ApplyUserIndexWithHost(pipe, mutex, error), "writer stops before checkpoint migration");
            check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0,
                "owned fixture host exits gracefully");
            check(storage::UserIndexRoot() == host_target.wstring(), "writer checkpoint activates custom root");
            check(fs::exists(host_target / L"pulse-index.bin"), "owned writer checkpoint survives relocation");
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    const auto profile = fixture / L"isolated-profile";
    fs::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    const auto config_root = profile / L"Pulse", config_target = fixture / L"content-config-moved";
    storage::OverrideDefaultRootForTesting(config_root.wstring());
    namespace sidecar = pulse::index::config_storage;
    const auto db = sidecar::DatabasePath();
    pulse::index::ContentIndexConfig content_config;
    content_config.maximum_document_bytes = 123456;
    check(sidecar::WriteConfigFile(db, content_config) &&
        fs::exists(config_root / L"content-index.config"), "content preferences use configuration directory");
    check(storage::Schedule(storage::Kind::Configuration, config_target.wstring(), error) &&
        storage::ApplyConfiguration(error), "content preferences migrate with application configuration");
    pulse::index::ContentIndexConfig restored;
    check(sidecar::ReadConfigFile(db, restored) && restored.maximum_document_bytes == 123456 &&
        fs::exists(config_target / L"content-index.config"), "content preferences load from relocated root");
    const auto custom_db = fixture / L"test-db.sqlite";
    check(sidecar::ConfigurationPath(custom_db.wstring()) == custom_db.wstring() + L".config",
        "explicit test database keeps its independent sidecar");
    // Fixtures are retained for inspection; the process never touches real storage.
    return failed ? 1 : 0;
}
