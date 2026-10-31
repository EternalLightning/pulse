// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/tab_transfer.h"
#include "../app/app_input.h"
#include "../app/app_navigation.h"
#include "../app/app_runtime.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

namespace pulse {
int RunWindowTransferRegressionChecks() {
    int failures = 0;
    const auto check = [&](bool passed, const char* description) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
        if (!passed) ++failures;
    };
    AppState source;
    AppState destination(source.core);
    source.isolatedTest = destination.isolatedTest = true;
    source.scale = 1.0f;
    destination.scale = 1.5f;
    source.window_tabs.EnsureDefault();
    destination.window_tabs.EnsureDefault();
    const auto instance = GetModuleHandleW(nullptr);
    const auto make_window = [&](int x) {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Transfer regression",
            WS_POPUP, x, -20000, 500, 500, nullptr, nullptr, instance, nullptr);
    };
    source.hwnd = make_window(-22000);
    destination.hwnd = make_window(-21000);
    const HWND cover = make_window(-21000);
    if (!source.hwnd || !destination.hwnd || !cover) {
        check(false, "create isolated transfer fixture windows");
        if (cover) DestroyWindow(cover);
        if (destination.hwnd) DestroyWindow(destination.hwnd);
        if (source.hwnd) DestroyWindow(source.hwnd);
        return failures;
    }
    source.core.windows.Add(source.hwnd, &source);
    source.core.windows.Add(destination.hwnd, &destination);
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    rows->resize(3);
    (*rows)[0].name = L"first.txt";
    (*rows)[1].name = L"second.txt";
    (*rows)[2].name = L"third.txt";
    const auto seed = [&](app::Tab& tab, float scroll) {
        tab.current_path = app::MakeRecentPath();
        tab.snapshot_path = tab.current_path;
        tab.snapshot = rows;
        tab.selected_index = 1;
        tab.selected = {1};
        tab.scroll_y = scroll;
        tab.scroll_x = 20.0f;
        tab.filter_text = L"second";
        tab.virtual_title = L"Retained recent results";
        tab.loading = false;
    };
    auto& source_layout = *source.window_tabs.Active();
    source_layout.panes.push_back(std::make_unique<app::Pane>());
    source_layout.layout = app::LayoutPreset::TwoVertical;
    app::RebuildLayoutRoot(source_layout);
    app::Pane* incoming = source_layout.panes.back().get();
    app::Pane* existing = destination.window_tabs.Active()->panes.front().get();
    seed(incoming->view, 160.0f);
    incoming->view.back_stack.push({L"C:\\history", {}, app::Tab::NavigationViewport{10, 40}});
    incoming->view.forward_stack.push({L"C:\\forward", {}, app::Tab::NavigationViewport{20, 60}});
    incoming->view.pending_history_viewport = app::Tab::NavigationViewport{30, 80};
    seed(existing->view, 96.0f);
    BindCurrentLayout(source);
    BindCurrentLayout(destination);
    check(TransferPane(source, 1, destination), "transfer one pane into an existing layout");
    check(existing->view.snapshot == rows && existing->view.selected_index == 1 &&
        existing->view.selected.contains(1) && existing->view.scroll_y == 96.0f &&
        existing->view.scroll_x == 20.0f && existing->view.filter_text == L"second" &&
        !existing->view.loading,
        "incoming pane leaves destination selection, viewport and virtual snapshot untouched");
    check(destination.pane == incoming && incoming->view.snapshot == rows &&
        incoming->view.selected.contains(1) && incoming->view.scroll_y == 240.0f &&
        incoming->view.scroll_x == 30.0f && incoming->view.virtual_title == L"Retained recent results" &&
        incoming->view.filter_text == L"second" && !incoming->view.loading,
        "transferred virtual pane keeps its live state and rescales its viewport for destination DPI");
    check(incoming->view.back_stack.top().viewport->y == 60 &&
        incoming->view.forward_stack.top().viewport->y == 90 &&
        incoming->view.pending_history_viewport->y == 120,
        "cross-DPI transfer also rescales Back, Forward and pending history viewports");

    const auto transfer_group = [&](const wchar_t* name, int expected_id, int expected_next,
                                    size_t expected_groups, bool collapsed = true) {
        source.window_tabs.tab_groups.push_back({5, name, 0x123456, collapsed});
        auto& layout = source.window_tabs.NewTab(app::MakeRecentPath());
        layout.tab_group = 5;
        seed(layout.panes.front()->view, 0.0f);
        const bool transferred = TransferLayoutTab(source, source.window_tabs.active, destination, false);
        const auto group = std::find_if(destination.window_tabs.tab_groups.begin(),
            destination.window_tabs.tab_groups.end(),
            [expected_id](const auto& value) { return value.id == expected_id; });
        return transferred && destination.window_tabs.Active() == &layout &&
            layout.tab_group == expected_id && destination.window_tabs.next_tab_group_id == expected_next &&
            destination.window_tabs.tab_groups.size() == expected_groups &&
            group != destination.window_tabs.tab_groups.end() && group->name == name &&
            group->color_rgb == 0x123456 && group->collapsed && source.window_tabs.tab_groups.empty();
    };
    destination.window_tabs.next_tab_group_id = 7;
    check(transfer_group(L"Transferred group", 7, 8, 1),
        "group transfer preserves membership, name, color and collapsed state");
    check(transfer_group(L"Transferred group", 7, 8, 1, false),
        "matching destination group is reused without consuming an ID or changing its collapsed state");
    const int maximum_group_id = (std::numeric_limits<int>::max)();
    destination.window_tabs.next_tab_group_id = maximum_group_id;
    check(transfer_group(L"Maximum group", maximum_group_id, 1, 2),
        "group transfer at INT_MAX wraps its next ID without signed overflow");
    destination.window_tabs.tab_groups.push_back({1, L"Occupied first group", 0, false});
    check(transfer_group(L"Wrapped group", 2, 3, 4),
        "group transfer after wrap skips an occupied ID and preserves existing groups");
    destination.window_tabs.next_tab_group_id = maximum_group_id;
    check(transfer_group(L"Occupied maximum group", 3, 4, 5),
        "group transfer safely skips an occupied INT_MAX and wrapped IDs");

    const auto folder = std::filesystem::absolute(L"bench_data/window-transfer-regression") /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(folder);
    std::ofstream(folder / L"first.txt") << "first";
    std::ofstream(folder / L"second.txt") << "second";
    std::ofstream(folder / L"third.txt") << "third";
    auto& directory_layout = source.window_tabs.NewTab(folder.wstring());
    seed(directory_layout.panes.front()->view, 80.0f);
    auto& directory_view = directory_layout.panes.front()->view;
    directory_view.current_path = directory_view.snapshot_path = folder.wstring();
    directory_view.pending_generation = 999999;
    directory_view.loading = true;
    const bool worker_started = destination.worker.Start([](app::WorkResult) {});
    check(worker_started, "start destination listing owner for a transferred pending directory");
    check(TransferLayoutTab(source, source.window_tabs.active, destination, false) &&
        destination.window_tabs.Active() == &directory_layout && directory_view.snapshot == rows &&
        directory_view.selected.contains(1) && directory_view.scroll_y == 120.0f &&
        directory_view.pending_generation != 0 && directory_view.pending_generation != 999999,
        "directory transfer preserves live state while replacing the source generation with destination work");
    destination.worker.Stop();
    std::error_code cleanup_error;
    std::filesystem::remove_all(folder, cleanup_error);
    check(!cleanup_error, "remove isolated transfer directory fixture");

    app::Tab pending_directory;
    pending_directory.current_path = fs::NormalizePath(folder.wstring());
    pending_directory.pending_selected_name = L"wanted.txt";
    pending_directory.pending_selected_names = {L"wanted.txt"};
    ResumeTransferredTab(destination, pending_directory);
    check(pending_directory.pending_selected_name == L"wanted.txt" &&
        pending_directory.pending_selected_names == std::vector<std::wstring>{L"wanted.txt"},
        "transferring a directory before its first snapshot preserves the pending selection intent");

    app::Tab paged_search;
    paged_search.current_path = paged_search.snapshot_path = L"pulse:search:needle";
    const auto loaded_rows = std::make_shared<std::vector<fs::DirEntry>>(900);
    paged_search.snapshot = loaded_rows;
    paged_search.search_next_offset = 900;
    paged_search.selected = {1, 800};
    paged_search.selected_index = 800;
    paged_search.scroll_y = 1600;
    ResumeTransferredTab(destination, paged_search);
    const auto request = destination.pendingIndexSearches.find(static_cast<uint32_t>(paged_search.pending_generation));
    check(request != destination.pendingIndexSearches.end() && request->second.query.limit >= 900 &&
        paged_search.snapshot == loaded_rows && paged_search.selected.contains(800) && paged_search.scroll_y == 1600,
        "reattached filename search requests all loaded pages without discarding selection or viewport");
    destination.index.CancelSession(paged_search.search_session_id);
    destination.networkIndex.CancelSession(paged_search.search_session_id);
    destination.pendingIndexSearches.clear();

    source.tabDragging = source.tabDragPending = true;
    source.tabDragIndex = 0;
    source.tabOrder = {0};
    source.paneDragging = true;
    source.paneDragLayoutIndex = 0;
    source.paneDragPaneIndex = 0;
    const auto source_tabs = source.window_tabs.items.size();
    const auto destination_tabs = destination.window_tabs.items.size();
    HandleKeyDown(&source, source.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!source.tabDragging && !source.tabDragPending && !source.paneDragging &&
        source.tabDragIndex == -1 && source.paneDragPaneIndex == -1 && source.tabOrder.empty(),
        "Esc clears tab and pane transfer gestures before normal key handling");
    // With the gesture disarmed, the production mouse-up dispatch cannot enter
    // either transfer branch. A late release must not move a tab or pane.
    HandleLButtonUp(&source, source.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(50, 50));
    check(source.window_tabs.items.size() == source_tabs &&
        destination.window_tabs.items.size() == destination_tabs,
        "mouse release after Esc does not transfer content");
    BeginWindowCaptionTransfer(source.hwnd);
    check(EndWindowCaptionTransfer(source.hwnd), "caption transfer accepts a completed gesture without Esc");
    BeginWindowCaptionTransfer(source.hwnd);
    PostMessageW(source.hwnd, WM_KEYDOWN, VK_ESCAPE, 1);
    MSG escape{};
    PeekMessageW(&escape, source.hwnd, WM_KEYDOWN, WM_KEYDOWN, PM_REMOVE);
    check(!EndWindowCaptionTransfer(source.hwnd),
        "caption move keyboard hook rejects an Esc-cancelled merge");

    ShowWindow(source.hwnd, SW_SHOWNOACTIVATE);
    ShowWindow(destination.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(destination.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    bool covered = false;
    const POINT drop{-20900, -19900};
    check(WindowTransferTargetAtPoint(source, drop, covered) == &destination && covered,
        "visible topmost Pulse window is a transfer target");
    ShowWindow(cover, SW_SHOWNOACTIVATE);
    SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    check(WindowTransferTargetAtPoint(source, drop, covered) == nullptr && covered,
        "a foreign top-level window blocks transfer into the Pulse window behind it");
    check(!IsWindowTransferDesktopSurface(cover),
        "a foreign tool window is not classified as a desktop drop surface");
    const HWND shell_desktop = GetShellWindow();
    if (shell_desktop)
        check(IsWindowTransferDesktopSurface(shell_desktop),
            "the actual shell desktop is classified as a detached-window drop surface");
    else
        std::printf("[SKIP] desktop surface check: no shell desktop in this session\n");
    ShowWindow(cover, SW_HIDE);
    const HWND overlap = make_window(-21000);
    AppState overlapping(source.core);
    overlapping.isolatedTest = true;
    overlapping.hwnd = overlap;
    source.core.windows.Add(overlap, &overlapping);
    ShowWindow(overlap, SW_SHOWNOACTIVATE);
    SetWindowPos(overlap, destination.hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    source.core.windows.Touch(overlap);
    check(WindowTransferTargetAtPoint(source, drop, covered) == &destination,
        "overlapping Pulse windows use desktop Z order instead of registry MRU order");
    CancelWindowCaptionTransfer(source.hwnd);
    source.core.windows.Remove(overlap);
    source.core.windows.Remove(source.hwnd);
    source.core.windows.Remove(destination.hwnd);
    if (overlap) DestroyWindow(overlap);
    DestroyWindow(cover);
    DestroyWindow(destination.hwnd);
    DestroyWindow(source.hwnd);
    overlapping.hwnd = destination.hwnd = source.hwnd = nullptr;
    std::printf("window transfer regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
} // namespace pulse
