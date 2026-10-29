#pragma once
#include "../app/app_column_view.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

inline bool RunColumnScrollChecks(pulse::AppState& s, const std::filesystem::path& root) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    s.scale = 1.0f;
    s.renderer.SetScale(s.scale);
    s.shot_high_contrast = false;
    auto& tab = *ActiveTab(s);
    tab.current_path = fs::NormalizePath(std::filesystem::absolute(root).wstring());
    const auto original_path = tab.current_path;
    tab.column_layout = true;
    tab.column_strip = {};
    tab.column_strip.for_path = tab.current_path;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(160);
    auto indices = std::make_shared<std::vector<int>>();
    for (size_t i = 0; i < rows->size(); ++i) {
        (*rows)[i].name = L"Row " + std::to_wstring(i);
        (*rows)[i].attrs = FILE_ATTRIBUTE_NORMAL;
        indices->push_back(static_cast<int>(i));
    }
    const auto make_column = [&](const std::wstring& path) {
        app::ColumnStripColumn col;
        col.path = path;
        col.snapshot = col.rows_snapshot = rows;
        col.rows = indices;
        col.requested = true;
        col.auto_scroll = false;
        return col;
    };
    for (int i = 0; i < 8; ++i)
        tab.column_strip.ancestors.push_back(make_column(tab.current_path + L"\\Ancestor " + std::to_wstring(i)));
    tab.column_strip.child = make_column(tab.current_path + L"\\Child");
    const auto make_frame = [&]() {
        ui::WindowViewModel vm;
        app::FillColumnStripView(vm.pane.column_strip, tab);
        vm.pane_slots.emplace_back();
        vm.pane_slots.front().rect = D2D1::RectF(0, 0, 1400, 600);
        vm.pane_slots.front().focused = true;
        vm.pane_slots.front().pane = vm.pane;
        return vm;
    };
    ui::HitTestResult hit;
    hit.region = ui::HitTestResult::ColumnStripBlank;
    hit.pane_index = 0;
    hit.sub_index = 7;
    auto vm = make_frame();
    auto& col = tab.column_strip.ancestors.back();
    uint64_t now = GetTickCount64();
    check(HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA) && col.scroll_dip == 0 && col.scroll_motion.Pending(),
        "column wheel starts a frame chain without jumping to its endpoint");
    check(TickColumnStripScroll(s, vm, now + 35) && col.scroll_dip > 0 &&
        col.scroll_dip < s.renderer.RowHeight() * 3,
        "column wheel advances through a genuine intermediate frame");
    const float first_middle = col.scroll_dip;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA);
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    check(col.scroll_dip > first_middle && col.scroll_motion.Pending(),
        "continuous column wheel input extends movement from the current offset");
    const float middle = col.scroll_dip;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, WHEEL_DELTA);
    check(col.scroll_motion.Pending(), "reverse column input retains an active frame chain");
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    check(col.scroll_dip < middle && col.scroll_dip >= 0,
        "reverse column input moves toward the new direction");
    now = GetTickCount64();
    for (uint64_t frame = 1; frame <= 30; ++frame) TickColumnStripScroll(s, vm, now + frame * 100);
    check(!col.scroll_motion.Pending() && col.scroll_dip >= 0,
        "column motion reaches a bounded endpoint and releases its frame chain");

    hit.sub_index = ui::kColumnStripChildId;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA);
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    check(tab.column_strip.child.scroll_dip > 0 && tab.column_strip.child.scroll_motion.Pending(),
        "selected-folder column uses the same smooth vertical input path");
    tab.column_strip.child.scroll_motion.Reset();

    hit.sub_index = 7;
    col.scroll_motion.Reset();
    col.scroll_dip = 300;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA);
    vm.pane_slots.front().rect.bottom = 20000;
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    check(col.scroll_dip == 0 && !col.scroll_motion.Pending(),
        "shrinking column scroll range clamps the offset and cancels momentum");
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA);
    InvalidateColumnStripPath(s, col.path, std::make_shared<std::vector<fs::DirEntry>>());
    SyncColumnStrips(s);
    vm = make_frame();
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    check(col.rows && col.rows->empty() && col.scroll_dip == 0 && !col.scroll_motion.Pending(),
        "replacement with an empty column cancels old content motion");

    vm = make_frame();
    hit.region = ui::HitTestResult::ColumnStripHScroll;
    HandleColumnStripWheel(s, vm, hit, WHEEL_DELTA);
    TickColumnStripScroll(s, vm, GetTickCount64() + 35);
    auto& strip = tab.column_strip;
    check(strip.scroll_from_right_dip > 0 && strip.scroll_from_right_dip < 96 && strip.horizontal_motion.Pending(),
        "horizontal strip wheel passes through an intermediate frame");
    const float horizontal_middle = strip.scroll_from_right_dip;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, WHEEL_DELTA, true);
    TickColumnStripScroll(s, vm, GetTickCount64() + 15);
    check(strip.scroll_from_right_dip < horizontal_middle,
        "native horizontal wheel reverses toward the right");
    vm = make_frame();
    auto layout = s.renderer.ColumnStripGeometry(vm.pane_slots.front().pane, vm.pane_slots.front().rect);
    D2D1_RECT_F track{}, thumb{};
    const bool has_scrollbar = s.renderer.ColumnStripHScrollRects(layout, track, thumb);
    check(has_scrollbar, "fixture has a real horizontal strip scrollbar");
    if (has_scrollbar) {
        HandleColumnStripMouseDown(s, vm, hit, static_cast<int>((thumb.left + thumb.right) * 0.5f));
        check(s.stripHScrolling && !strip.horizontal_motion.Pending(),
            "thumb capture cancels horizontal residual motion");
        const float drag_start = strip.scroll_from_right_dip;
        UpdateColumnStripResize(s, s.stripResizeStartX - 20);
        const float dragged = strip.scroll_from_right_dip;
        vm = make_frame();
        TickColumnStripScroll(s, vm, GetTickCount64() + 40);
        check(dragged > drag_start && strip.scroll_from_right_dip == dragged && !strip.horizontal_motion.Pending(),
            "thumb dragging moves immediately and leaves no animated drift");
        EndColumnStripResize(s);
        ReleaseCapture();
    }
    strip.horizontal_motion.Reset();
    strip.scroll_from_right_dip = 0;
    vm = make_frame();
    layout = s.renderer.ColumnStripGeometry(vm.pane_slots.front().pane, vm.pane_slots.front().rect);
    if (s.renderer.ColumnStripHScrollRects(layout, track, thumb)) {
        HandleColumnStripMouseDown(s, vm, hit, static_cast<int>(track.left + 1));
        TickColumnStripScroll(s, vm, GetTickCount64() + 35);
        check(strip.scroll_from_right_dip > 0 && strip.scroll_from_right_dip < layout.viewport.right - layout.viewport.left &&
            strip.horizontal_motion.Pending(), "horizontal track paging animates between page endpoints");
    }
    s.shot_high_contrast = true;
    TickColumnStripScroll(s, vm, GetTickCount64() + 50);
    check(!strip.horizontal_motion.Pending(), "disabled motion settles an existing horizontal frame chain");
    strip.scroll_from_right_dip = 0;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, WHEEL_DELTA);
    check(strip.scroll_from_right_dip == 96 && !strip.horizontal_motion.Pending(),
        "disabled motion applies horizontal wheel distance immediately");
    hit.region = ui::HitTestResult::ColumnStripBlank;
    hit.sub_index = 6;
    auto& other_col = strip.ancestors[6];
    other_col.scroll_dip = 0;
    vm = make_frame();
    HandleColumnStripWheel(s, vm, hit, -WHEEL_DELTA);
    check(other_col.scroll_dip == 3 * s.renderer.RowHeight() && !other_col.scroll_motion.Pending(),
        "disabled motion applies vertical wheel distance immediately");
    check(tab.current_path == original_path, "scroll inputs preserve the current navigation path");
    s.shot_high_contrast = false;
    return failures == 0;
}
