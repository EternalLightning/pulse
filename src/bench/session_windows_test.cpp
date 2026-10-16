#include "../app/session.h"
#include <iostream>
#include <string_view>

namespace {
using pulse::app::LayoutTabSnapshot;
using pulse::app::PaneFolderSnapshot;
using pulse::app::SessionSnapshot;
using pulse::app::WindowSessionSnapshot;

bool Report(bool passed, const char* name) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
    return passed;
}

WindowSessionSnapshot MakeWindow(int index) {
    WindowSessionSnapshot window;
    window.window_rect = {100 + index * 900, 80 + index * 40,
        1100 + index * 900, 780 + index * 40};
    window.maximized = index == 1;
    window.dark = index != 0;
    window.active_path = index == 0 ? L"C:\\work\\alpha" : L"D:\\work\\beta";
    window.sidebar_collapsed = 1 << index;
    window.sidebar_hidden = 1 << (index + 1);
    window.sidebar_order = {0, 2, 1, 3, 4, 5, 6};
    window.quick_access_hidden = index == 0 ? 5 : 10;
    window.starred_expanded = index == 0;
    window.details_panel = index != 0;
    window.details_panel_width = 360 + index * 20;
    window.details_preview_only = index == 1;
    window.details_preview = index == 0;
    window.active_layout_tab = 1;
    window.tab_groups = {{7 + index, L"group-" + std::to_wstring(index), 0x123456u + static_cast<uint32_t>(index), index != 0}};

    LayoutTabSnapshot first;
    first.title = L"first-" + std::to_wstring(index);
    first.marker_rgb = 0x224466;
    first.pinned = true;
    first.group = 7 + index;
    first.layout = 1;
    first.focused = 1;
    first.target = 0;
    first.split_ratios = {0.35f, 0.65f};
    PaneFolderSnapshot left;
    left.path = window.active_path;
    left.view = pulse::ui::ViewMode::Details;
    left.columns = {0.2f, 0.55f, 0.8f};
    left.search_columns = {0.15f, 0.4f, 0.7f, 0.9f};
    left.column_layout = true;
    left.column_widths = {120.0f, 240.0f, 380.0f};
    PaneFolderSnapshot right = left;
    right.path += L"\\nested";
    right.view = pulse::ui::ViewMode::List;
    right.column_widths = {96.0f, 192.0f};
    first.panes = {left, right};

    LayoutTabSnapshot second;
    second.title = L"selected-" + std::to_wstring(index);
    second.layout = 0;
    second.focused = 0;
    PaneFolderSnapshot selected;
    selected.path = window.active_path + L"\\selected";
    selected.view = pulse::ui::ViewMode::Tiles;
    selected.columns = {0.1f, 0.5f, 0.9f};
    second.panes.push_back(selected);
    window.layout_tabs = {first, second};
    return window;
}

bool SameWindowRoundTrip(const WindowSessionSnapshot& actual, const WindowSessionSnapshot& expected) {
    if (actual.window_rect.left != expected.window_rect.left ||
        actual.window_rect.top != expected.window_rect.top ||
        actual.window_rect.right != expected.window_rect.right ||
        actual.window_rect.bottom != expected.window_rect.bottom ||
        actual.maximized != expected.maximized || actual.active_path != expected.active_path ||
        actual.quick_access_hidden != expected.quick_access_hidden ||
        actual.active_layout_tab != expected.active_layout_tab ||
        actual.tab_groups.size() != 1 || actual.tab_groups[0].name != expected.tab_groups[0].name ||
        actual.layout_tabs.size() != 2) return false;
    const auto& tab = actual.layout_tabs[0];
    return tab.pinned == expected.layout_tabs[0].pinned && tab.group == expected.layout_tabs[0].group &&
        tab.layout == expected.layout_tabs[0].layout && tab.focused == expected.layout_tabs[0].focused &&
        tab.target == expected.layout_tabs[0].target && tab.split_ratios == expected.layout_tabs[0].split_ratios &&
        tab.panes.size() == 2 && tab.panes[0].path == expected.layout_tabs[0].panes[0].path &&
        tab.panes[1].path == expected.layout_tabs[0].panes[1].path &&
        tab.panes[0].columns == expected.layout_tabs[0].panes[0].columns &&
        tab.panes[0].search_columns == expected.layout_tabs[0].panes[0].search_columns &&
        tab.panes[0].column_layout == expected.layout_tabs[0].panes[0].column_layout &&
        tab.panes[0].column_widths == expected.layout_tabs[0].panes[0].column_widths &&
        tab.panes[1].view == expected.layout_tabs[0].panes[1].view &&
        actual.layout_tabs[1].title == expected.layout_tabs[1].title;
}
}

int wmain() {
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        failures += !Report(passed, name);
    };

    const auto first = MakeWindow(0);
    const auto second = MakeWindow(1);
    SessionSnapshot multi;
    multi.windows = {first, second};
    // Deliberately stale legacy projection: the first window must be projected at serialization.
    multi.window_rect = {1, 2, 3, 4};
    multi.active_path = L"stale projection";
    const auto multi_json = pulse::app::SessionToJson(multi);
    SessionSnapshot restored;
    check(pulse::app::ParseSessionJson(multi_json, restored) && restored.windows.size() == 2 &&
        SameWindowRoundTrip(restored.windows[0], first) && SameWindowRoundTrip(restored.windows[1], second) &&
        restored.active_path == first.active_path && restored.window_rect.left == first.window_rect.left &&
        restored.maximized == first.maximized,
        "two windows round-trip independent geometry, maximization, layouts, panes, groups, columns, and active selection");

    SessionSnapshot legacy_base;
    static_cast<WindowSessionSnapshot&>(legacy_base) = first;
    legacy_base.windows.clear();
    const auto synthesized_json = pulse::app::SessionToJson(legacy_base);
    check(pulse::app::ParseSessionJson(synthesized_json, restored) && restored.windows.size() == 1 &&
        SameWindowRoundTrip(restored.windows.front(), first),
        "an empty windows vector serializes the legacy base as one window");

    const std::wstring version_seven = LR"({"version":7,"left":15,"top":25,"right":815,"bottom":625,"maximized":true,"dark":false,"path":"C:\\legacy","quickAccessHidden":5,"tray":[],"undo":[],"layoutTabs":[{"title":"legacy tab","panes":[{"path":"C:\\legacy\\pane","view":"details","cols":"0.2,0.5,0.8"}]}]})";
    check(pulse::app::ParseSessionJson(version_seven, restored) && restored.windows.size() == 1 &&
        restored.windows[0].window_rect.left == 15 && restored.windows[0].active_path == L"C:\\legacy" &&
        restored.windows[0].quick_access_hidden == 5 && restored.quick_access_hidden == 5,
        "version 7 data restores as one window and keeps its quick access hidden bits");

    const std::wstring malformed_windows = LR"({"version":8,"left":41,"top":42,"right":941,"bottom":742,"path":"C:\\root","quickAccessHidden":3,"windows":[null]})";
    check(pulse::app::ParseSessionJson(malformed_windows, restored) && restored.windows.size() == 1 &&
        restored.windows[0].window_rect.left == 41 && restored.windows[0].active_path == L"C:\\root",
        "an invalid windows entry falls back to the valid top-level single-window state");
    const std::wstring malformed_array = LR"({"version":8,"left":51,"top":52,"right":951,"bottom":752,"path":"D:\\root","windows":"invalid"})";
    check(pulse::app::ParseSessionJson(malformed_array, restored) && restored.windows.size() == 1 &&
        restored.windows[0].window_rect.left == 51 && restored.windows[0].active_path == L"D:\\root",
        "a malformed windows array falls back to the valid top-level single-window state");

    multi.windows.clear();
    for (int index = 0; index < 33; ++index) {
        auto window = MakeWindow(index);
        window.active_path = L"window-" + std::to_wstring(index);
        multi.windows.push_back(std::move(window));
    }
    check(pulse::app::ParseSessionJson(pulse::app::SessionToJson(multi), restored) &&
        restored.windows.size() == 32 && restored.windows.back().active_path == L"window-31",
        "session serialization caps restored windows at 32");

    std::cout << "[INFO] failures=" << failures << '\n';
    return failures ? 1 : 0;
}
