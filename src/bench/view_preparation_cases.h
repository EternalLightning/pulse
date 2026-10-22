#pragma once

#include "../app/app_internal.h"
#include "../app/folder_sizes_ui.h"
#include <cstdio>

inline int RunViewPreparationCases(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto& tab = *ActiveTab(s);
    tab.current_path = L"pulse:view-preparation";
    tab.SetShowHiddenFiles(false);
    tab.SetShowProtectedOsFiles(false);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (size_t i = 0; i < entries->size(); ++i) {
        auto& entry = (*entries)[i];
        entry.name = std::to_wstring(i) + L".txt";
        entry.full_path = L"pulse:view-preparation:" + entry.name;
        entry.attrs = i == 0 ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_HIDDEN;
        if (i == 2) entry.attrs |= FILE_ATTRIBUTE_SYSTEM;
        entry.mtime.dwLowDateTime = static_cast<DWORD>(100 + i);
    }
    tab.SetSnapshot(entries);
    tab.SelectOnly(0);
    s.appPrefs.show_hidden_files = true;
    s.appPrefs.show_protected_os_files = true;
    s.showDetailsPanel = true;
    s.detailsSelPath = L"pulse:previous-selection";
    s.detailsRequestId = 40;
    s.detailsSelValid = true;
    s.detailsOwner = L"prepared owner";
    s.detailsModified.dwLowDateTime = 99;
    s.detailsScroll = 25.0f;
    tab.column_strip.for_path = L"prepared columns";
    const auto initial_generation = tab.view_generation;
    SetWindowTextW(s.hwnd, L"Prepared title");
    s.windowTitleCache = L"Prepared title";
    for (int rebuild = 0; rebuild < 2; ++rebuild) {
        s.hitTestVmValid = false;
        const auto& hit_vm = HitTestVm(s);
        check(hit_vm.pane.EntryCount() == 1 && hit_vm.details.has_selection,
            "hit-test rebuild reads the current prepared projection");
    }
    (void)BuildVm(s);
    wchar_t title[128]{};
    GetWindowTextW(s.hwnd, title, static_cast<int>(std::size(title)));
    check(std::wstring(title) == L"Prepared title" && s.windowTitleCache == title,
        "hit-test and display model reads do not synchronize the native title");
    check(!tab.show_hidden_files && !tab.show_protected_os_files &&
        tab.view_generation == initial_generation && tab.SelectedCount() == 1 &&
        tab.column_strip.for_path == L"prepared columns",
        "model reads do not apply preferences, change selection, or synchronize columns");
    check(s.detailsRequestId == 40 && s.detailsSelPath == L"pulse:previous-selection" &&
        s.detailsSelValid && s.detailsOwner == L"prepared owner" &&
        s.detailsModified.dwLowDateTime == 99 && s.detailsScroll == 25.0f,
        "model reads do not advance requests or change prepared details facts");

    PrepareViewState(s);
    GetWindowTextW(s.hwnd, title, static_cast<int>(std::size(title)));
    check(std::wstring(title) == app::LayoutTabTitle(*s.window_tabs.Active()) + L" - Pulse" &&
        tab.show_hidden_files && tab.show_protected_os_files && tab.EntryCount() == 3 &&
        tab.column_strip.for_path.empty(),
        "explicit preparation applies title, hidden/protected preferences, and column state");
    check(tab.SelectedCount() == 0 && s.detailsSelPath.empty() && s.detailsRequestId == 41,
        "preference-driven selection clearing invalidates the previous details request");

    tab.SelectOnly(0);
    PrepareViewState(s);
    const auto first_request = s.detailsRequestId;
    check(s.detailsSelPath == (*entries)[0].full_path && !s.detailsSelValid &&
        s.detailsOwner.empty() && s.detailsScroll == 0.0f,
        "programmatic selection prepares its details version and clears old facts");
    PrepareViewState(s);
    check(s.detailsRequestId == first_request,
        "unchanged selection and snapshot do not submit another details request");

    auto refreshed = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    (*refreshed)[0].mtime.dwLowDateTime = 555;
    tab.SetSnapshot(refreshed);
    s.detailsSelValid = true;
    s.detailsMetaPath = s.detailsSelPath;
    s.detailsModified.dwLowDateTime = 100;
    s.hitTestVmValid = false;
    (void)HitTestVm(s);
    check(s.detailsRequestId == first_request && s.detailsModified.dwLowDateTime == 100 &&
        s.detailsSelValid, "snapshot changes wait for preparation instead of advancing during hit tests");
    PrepareViewState(s);
    check(s.detailsRequestId == first_request + 1 && s.detailsModified.dwLowDateTime == 555 &&
        !s.detailsSelValid && s.detailsProbeSnapshot.lock() == tab.snapshot,
        "same-path snapshot replacement refreshes facts and advances the details version");
    DetailsMetaResult stale;
    stale.path = s.detailsSelPath;
    stale.window_epoch = s.detailsWindowEpoch;
    stale.request_id = first_request;
    check(!ApplyDetailsMetaResult(s, stale), "preparation rejects the replaced snapshot's late metadata");

    tab.SelectOnly(1);
    ++tab.view_generation;
    s.detailsScroll = 20.0f;
    PrepareViewState(s);
    check(s.detailsSelPath == (*entries)[1].full_path && s.detailsRequestId == first_request + 2 &&
        s.detailsScroll == 0.0f && s.detailsProbeViewGeneration == tab.view_generation,
        "programmatic selection and view changes prepare the new path before drawing");
    const auto display = BuildVm(s, false);
    s.hitTestVmValid = false;
    const auto& hit = HitTestVm(s);
    check(hit.details.path == display.details.path && hit.details.scroll_y == display.details.scroll_y &&
        hit.pane.EntryCount() == display.pane.EntryCount() &&
        hit.pane_slots.size() == display.pane_slots.size() &&
        hit.sidebar_scroll == display.sidebar_scroll,
        "drawing and hit-test models agree after explicit preparation");

    const auto selected_request = s.detailsRequestId;
    tab.SelectAll();
    PrepareViewState(s);
    check(s.detailsSelPath.empty() && s.detailsRequestId == selected_request + 1 &&
        BuildVm(s, false).details.multi_count == 3,
        "multiple selection invalidates single-item details while retaining the summary");
    tab.SelectOnly(0);
    PrepareViewState(s);
    const auto visible_request = s.detailsRequestId;
    s.showDetailsPanel = false;
    PrepareViewState(s);
    check(s.detailsSelPath.empty() && s.detailsRequestId == visible_request + 1,
        "hiding details invalidates the outstanding metadata version");

    tab.current_path = app::MakeSettingsPath(L"duplicates");
    s.dup_volumes_mailbox = std::make_shared<app::WindowResultMailbox<std::vector<index::VolumeInfo>>>(
        s.hwnd, WM_DUP_VOLUMES);
    s.dup_volume_cache_tick = 0;
    s.dup_volume_cache_pending = false;
    s.hitTestVmValid = false;
    (void)HitTestVm(s);
    check(!s.dup_volume_cache_pending && s.dup_volume_cache_tick == 0,
        "duplicate settings hit-test reads do not start background volume discovery");
    s.dup_volumes_mailbox->Close();
    s.dup_volumes_mailbox.reset();

    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const auto root = std::wstring(cwd) + L"\\bench_data\\view-preparation-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto child = root + L"\\child";
    const bool made_root = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
    const bool made_child = made_root && CreateDirectoryW(child.c_str(), nullptr) != FALSE;
    check(made_child, "create an isolated visible folder-size fixture");
    if (made_child) {
        tab.current_path = root;
        tab.view_mode = ui::ViewMode::Tiles;
        tab.loading = false;
        auto folders = std::make_shared<std::vector<fs::DirEntry>>(1);
        folders->front().name = L"child";
        folders->front().full_path = child;
        folders->front().is_dir = true;
        folders->front().attrs = FILE_ATTRIBUTE_DIRECTORY;
        tab.SetSnapshot(folders);
        tab.ClearSelection();
        s.hitTestVmValid = false;
        (void)HitTestVm(s);
        auto display_folders = BuildVm(s, false);
        s.folderSizes.Calculate(child);
        check(s.folderSizes.Get(child).state == app::FolderSizeState::Manual,
            "hit-test and display model reads do not register visible folder-size requests");
        PrepareFolderSizes(s, display_folders);
        s.folderSizes.Calculate(child);
        check(s.folderSizes.Get(child).state != app::FolderSizeState::Manual,
            "explicit folder preparation registers the visible directory for calculation");
        s.folderSizes.Sync({{root, false}}, {});
        tab.current_path = L"pulse:view-preparation";
        tab.SetSnapshot({});
        s.hitTestVmValid = false;
        (void)HitTestVm(s);
        s.folderSizes.Calculate(root);
        check(s.folderSizes.Get(root).state != app::FolderSizeState::Manual,
            "hit-test rebuild does not cancel already prepared folder-size interests");
        s.folderSizes.Stop();
        check(RemoveDirectoryW(child.c_str()) != FALSE, "remove the isolated child directory");
    }
    if (made_root)
        check(RemoveDirectoryW(root.c_str()) != FALSE, "remove the isolated folder-size fixture");
    return failures ? 1 : 0;
}
