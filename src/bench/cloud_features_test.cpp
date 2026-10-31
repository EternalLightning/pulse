// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../fs/cloud_state.h"
#include "../fs/cloud_hydrate.h"
#include "../fs/cloud_pin.h"
#include "../app/cloud_roots.h"
#include "../app/cloud_root_catalog.h"
#include "../app/cloud_sync_probe.h"
#include "../app/cloud_download.h"
#include "../ui/cloud_status.h"
#include "cloud_probe_access_cases.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <shellapi.h>

int main(int argc, char** argv) {
    using namespace pulse;
    int failed = 0;
    const auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failed;
    };
    if (argc == 2 && std::string_view(argv[1]) == "--sync-probe-only") {
        bench::CloudProbeAccessCases(check);
        return failed ? 1 : 0;
    }
    if (argc > 2 && std::string_view(argv[1]) == "--system-roots-only") {
        int count = 0;
        auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments) return 2;
        std::vector<app::CloudRoot> seeds;
        for (int i = 2; i < count; ++i)
            seeds.push_back({arguments[i], L"Account", {}, app::CloudRootSource::Seed});
        LocalFree(arguments);
        app::RequestCloudRootRefresh(seeds);
        const auto until = GetTickCount64() + 10000;
        auto roots = app::CloudRootCatalog();
        while (!roots && GetTickCount64() < until) {
            Sleep(20);
            roots = app::CloudRootCatalog();
        }
        bool unique = roots != nullptr;
        bool covered = roots != nullptr;
        if (roots) {
            std::cout << "Final system catalog rows: " << roots->size() << std::endl;
            for (size_t i = 0; i < roots->size(); ++i)
                for (size_t j = 0; j < i; ++j)
                    unique &= !app::SameCloudRootPath((*roots)[i].path, (*roots)[j].path);
            for (const auto& seed : seeds) {
                size_t matches = 0;
                for (const auto& root : *roots)
                    if (app::SameCloudRootPath(seed.path, root.path)) ++matches;
                covered &= matches == 1;
            }
        }
        check(unique, "real background catalog contains no duplicate folder roots");
        check(covered, "each real account seed appears exactly once after the background refresh");
        return failed ? 1 : 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--roots-only") {
        check(app::SameCloudRootPath(L"\\\\?\\E:\\OneDrive", L"e:/OneDrive/"),
            "account extended path matches the Explorer sync root");
        check(app::SameCloudRootPath(L"\\\\?\\uNc\\server\\share\\Cloud\\", L"\\\\SERVER\\share\\Cloud"),
            "extended UNC roots match ordinary UNC roots");
        check(!app::SameCloudRootPath(L"E:\\OneDrive", L"E:\\OneDrive - Work"),
            "different accounts retain separate roots");
        std::vector<app::CloudRoot> roots;
        const auto directory = [](const std::wstring&) { return true; };
        app::MergeCloudRoots(roots, {{L"E:\\OneDrive - Work", L"Work", L"namespace-work"},
            {L"\\\\?\\E:\\OneDrive - Work\\", L"Duplicate", L"namespace-duplicate"}}, directory);
        check(roots.size() == 1 && roots.front().label == L"Work",
            "duplicate namespace registrations keep the first display label");
        app::MergeCloudRoots(roots, {{L"E:\\OneDrive", L"Personal", L"sync-personal", app::CloudRootSource::SyncRoot},
            {L"E:\\OneDrive - Work", L"Work sync", L"sync-work", app::CloudRootSource::SyncRoot}}, directory);
        app::MergeCloudRoots(roots, {{L"\\\\?\\E:\\OneDrive", L"Personal seed", {}, app::CloudRootSource::Seed},
            {L"\\\\?\\E:\\OneDrive - Work", L"Work seed", {}, app::CloudRootSource::Seed}}, directory);
        check(roots.size() == 2 && roots[0].label == L"Work" && roots[1].label == L"Personal",
            "two accounts across namespace sync and extended seeds produce two rows instead of four");
        app::MergeCloudRoots(roots, {{L"E:\\OneDrive\\Nested", L"Nested"}}, directory);
        check(roots.size() == 3, "distinct nested sync roots are preserved");
        app::MergeCloudRoots(roots, {{L"E:\\Missing", L"Missing"}},
            [](const std::wstring&) { return false; });
        check(roots.size() == 3, "unavailable registrations are excluded");
        app::PublishCloudFilesRoots({L"\\\\?\\E:\\OneDrive"});
        app::PublishCloudRootLabels({{L"\\\\?\\E:\\OneDrive", L"Personal"}});
        check(app::IsUnderCloudFilesRoot(L"E:\\OneDrive\\file") &&
            app::CloudRootLabelFor(L"\\\\?\\E:\\OneDrive\\file") == L"Personal",
            "normalized extended roots retain status and provider lookup");
        std::vector<app::CloudRoot> mixed;
        app::MergeCloudRoots(mixed, {
            {L"E:\\MiCloud", L"小米云盘", L"{fixture-xiaomi}", app::CloudRootSource::NavigationPane, 0x10},
            {L"E:\\Work", L"OneDrive - Nazo Game", L"{fixture-work}"}}, directory);
        app::MergeCloudRoots(mixed, {
            {L"E:\\Personal", L"OneDrive - Personal", L"OneDrive!sid!Personal", app::CloudRootSource::SyncRoot}}, directory);
        app::SortCloudRoots(mixed);
        check(mixed.size() == 3 && mixed[0].path == L"E:\\Work" &&
            mixed[1].path == L"E:\\Personal" && mixed[2].path == L"E:\\MiCloud",
            "merged namespace and sync roots keep OneDrive accounts together before Xiaomi");
        std::vector<app::CloudRoot> localized{{L"E:\\Work", L"本地化账户", L"{fixture-work}"}};
        app::MergeCloudRoots(localized, {
            {L"\\\\?\\E:\\Work", L"Account", {}, app::CloudRootSource::Seed, 0x42, true}}, directory);
        check(localized.size() == 1 && app::IsOneDriveCloudRoot(localized.front()) &&
            localized.front().label == L"本地化账户",
            "account seeds retain OneDrive identity without replacing the registered label");
        check(!app::IsOneDriveCloudRoot(mixed.back()) &&
            !app::IsOneDriveCloudRoot({L"E:\\OneDrive", L"My OneDrive backup", L"Other!sid!Account"}) &&
            app::IsOneDriveCloudRoot({L"E:\\Personal", L"个人账户", L"onedrive!sid!Personal"}),
            "only provider identity or a branded display name selects the OneDrive icon");
        std::vector<app::CloudRoot> ordered{
            {L"E:\\z", L"Zulu", {}, app::CloudRootSource::SyncRoot, 0x42},
            {L"E:\\b", L"alpha", {}, app::CloudRootSource::Seed, 0x42},
            {L"E:\\first", L"First", {}, app::CloudRootSource::NavigationPane, 0x20},
            {L"E:\\a", L"ALPHA", {}, app::CloudRootSource::NavigationPane, 0x42}};
        app::SortCloudRoots(ordered);
        check(ordered[0].path == L"E:\\first" && ordered[1].path == L"E:\\a" &&
            ordered[2].path == L"E:\\b" && ordered[3].path == L"E:\\z",
            "other providers sort by registered order then case-insensitive name and path");
        return failed ? 1 : 0;
    }
    check(!fs::IsCloudPlaceholder(FILE_ATTRIBUTE_NORMAL), "local files do not require hydration");
    for (DWORD attr : {DWORD(FILE_ATTRIBUTE_OFFLINE), fs::kAttrRecallOnOpen, fs::kAttrRecallOnDataAccess})
        check(fs::IsCloudPlaceholder(attr | fs::kAttrPinned) &&
            ui::CloudStatusLabel(attr | fs::kAttrPinned, fs::CloudSync::Idle) == l10n::StringId::CloudOnlineOnly,
            "recall flags take precedence over a not-yet-downloaded pin");
    check(ui::CloudStatusLabel(fs::kAttrPinned, fs::CloudSync::Idle) == l10n::StringId::CloudKept &&
        ui::CloudStatusLabel(0, fs::CloudSync::Idle) == l10n::StringId::CloudAvailableLocal,
        "local and pinned status labels differ");
    check(ui::CloudStatusLabel(fs::kAttrPinned, fs::CloudSync::Error) == l10n::StringId::CloudSyncError &&
        ui::CloudStatusLabel(0, fs::CloudSync::Syncing) == l10n::StringId::CloudSyncing,
        "provider transfer and error status override availability");
    check(ui::CloudStatusColumnWidth(199, 1) == 0 && ui::CloudStatusColumnWidth(200, 1) == 64 &&
        ui::CloudStatusColumnWidth(300, 1.5f) == 96,
        "status column follows DPI and hides when the pane cannot fit it");
    check(fs::CloudPinAttrs(fs::kAttrUnpinned | FILE_ATTRIBUTE_ARCHIVE, fs::CloudPinAction::Keep) ==
        (fs::kAttrPinned | FILE_ATTRIBUTE_ARCHIVE) &&
        fs::CloudPinAttrs(fs::kAttrPinned | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
            fs::CloudPinAction::Free) == fs::kAttrUnpinned,
        "pin changes preserve writable metadata and clear the opposite state");
    app::PublishCloudFilesRoots({L"C:\\Cloud", L"D:\\Cloud"});
    check(app::IsUnderCloudFilesRoot(L"c:\\cloud\\nested\\file") &&
        !app::IsUnderCloudFilesRoot(L"C:\\Cloud-other\\file"),
        "cloud root matching respects path boundaries and case");
    app::PublishCloudRootLabels({{L"C:\\Cloud", L"Root"}, {L"C:\\Cloud\\nested", L"Nested"}});
    check(app::CloudRootLabelFor(L"C:\\Cloud\\nested\\file") == L"Nested",
        "nested sync root supplies its own provider label");
    uint32_t provider = 6;
    int queries = 0;
    app::CloudSyncProbe probe([&](const std::wstring&) { ++queries; return provider; },
        app::CloudSyncProbe::Mode::Manual);
    check(probe.Lookup(L"fixture", 1) == fs::CloudSync::Idle && probe.QueuedForTest() == 1 && !queries,
        "UI lookup queues metadata without calling a provider");
    probe.Lookup(L"fixture", 1);
    check(probe.QueuedForTest() == 1 && probe.RunPendingForTest() == 1 && queries == 1,
        "duplicate visible rows share one metadata request");
    check(probe.Lookup(L"fixture", 1) == fs::CloudSync::Syncing,
        "background provider results update visible sync status");
    provider = 7;
    probe.Lookup(L"fixture", 2);
    probe.RunPendingForTest();
    check(probe.Lookup(L"fixture", 2) == fs::CloudSync::Error,
        "modified entries invalidate cached provider state");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"cloud-features-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root);
    const auto file = root / L"content.bin";
    const std::string bytes(3 * 1024 * 1024, 'x');
    { std::ofstream out(file, std::ios::binary); out.write(bytes.data(), bytes.size()); }
    std::atomic_bool cancel{false};
    uint64_t read = 0;
    DWORD error = 0;
    check(fs::HydrateByReading(file.wstring(), cancel, [&](uint64_t value) { read = value; }, &error) ==
        fs::HydrateResult::Done && read == bytes.size() && !error && std::filesystem::file_size(file) == bytes.size(),
        "explicit hydration reads complete content without modifying it");
    cancel = true;
    check(fs::HydrateByReading(file.wstring(), cancel, {}, &error) == fs::HydrateResult::Cancelled &&
        fs::ApplyCloudPin({file.wstring()}, fs::CloudPinAction::Free, &cancel).cancelled,
        "canceled hydration and pin operations stop before accessing content");
    cancel = false;
    check(fs::HydrateByReading((root / L"missing").wstring(), cancel, {}, &error) ==
        fs::HydrateResult::Failed && error == ERROR_FILE_NOT_FOUND,
        "hydration failures retain the operating system error");
    check(fs::HydrateByReading(file.wstring(), cancel, [&](uint64_t) { cancel = true; }, &error) ==
        fs::HydrateResult::Cancelled, "active hydration observes cancellation between chunks");
    const HWND first_window = CreateWindowExW(0, L"STATIC", L"first download", 0, 0, 0, 1, 1,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND second_window = CreateWindowExW(0, L"STATIC", L"second download", 0, 0, 0, 1, 1,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    app::CloudDownloads first, second;
    constexpr UINT message = WM_APP + 110;
    check(first_window && second_window && first.Start(file.wstring(), bytes.size(), first_window, message) &&
        second.Start(file.wstring(), bytes.size(), second_window, message),
        "two windows can start independent downloads of the same path");
    first.Close();
    bool second_done = false;
    const auto until = GetTickCount64() + 5000;
    while (!second_done && GetTickCount64() < until) {
        MSG event{};
        while (PeekMessageW(&event, second_window, message, message, PM_REMOVE)) {
            for (const auto& result : second.Take(event.wParam)) {
                if (result.event == app::CloudDownloadEvent::Done)
                    second_done = result.attrs != INVALID_FILE_ATTRIBUTES && result.size == bytes.size();
            }
        }
        if (!second_done) MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_POSTMESSAGE);
    }
    check(second_done, "closing one download window leaves the other completion and metadata intact");
    second.Close();
    DestroyWindow(first_window);
    DestroyWindow(second_window);
    std::filesystem::remove_all(root);
    return failed ? 1 : 0;
}
