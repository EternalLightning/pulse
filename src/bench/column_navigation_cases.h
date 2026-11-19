// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once

#include "animation_snapshot.h"
#include "../app/app_column_view.h"
#include <algorithm>
#include <cmath>
#include <fstream>

inline bool RunColumnNavigationChecks(pulse::AppState& s, const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto root = std::filesystem::absolute(output / L"column-navigation");
    const auto alpha = root / L"Alpha";
    const auto beta = root / L"Beta";
    const auto deep = alpha / L"One" / L"Two" / L"Three";
    std::filesystem::create_directories(deep);
    std::filesystem::create_directories(beta);
    std::filesystem::create_directories(beta / L"Child");
    { std::ofstream file(root / L"ordinary.txt"); file << "column fixture"; }
    const auto normalized_root = fs::NormalizePath(root.wstring());
    const auto normalized_beta = fs::NormalizePath(beta.wstring());
    const auto normalized_deep = fs::NormalizePath(deep.wstring());
    auto listing = std::make_shared<std::vector<fs::DirEntry>>();
    for (const wchar_t* name : {L"Alpha", L"Beta", L"ordinary.txt"}) {
        fs::DirEntry entry;
        entry.name = name;
        entry.is_dir = entry.name != L"ordinary.txt";
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        listing->push_back(std::move(entry));
    }
    // Seed only isolated fixture data: no user directory is enumerated by this test.
    auto empty = std::make_shared<const std::vector<fs::DirEntry>>();
    for (std::wstring path = normalized_deep; !path.empty();) {
        s.store.Put(path, path == normalized_root ? listing : empty);
        const auto parent = fs::ParentPath(path);
        if (parent == path) break;
        path = parent;
    }
    s.store.Put(normalized_beta, empty);
    s.store.Put(fs::NormalizePath((beta / L"Child").wstring()), empty);
    auto& tab = *ActiveTab(s);
    tab.column_layout = true;
    s.renderer.SetDetailsPanelVisible(false);
    const auto frame_bounds = [&](const ui::WindowViewModel& vm) {
        return vm.pane_slots.empty() ? s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height())) : vm.pane_slots.front().rect;
    };
    const auto frame_pane = [](const ui::WindowViewModel& vm) -> const ui::PaneViewModel& {
        return vm.pane_slots.empty() ? vm.pane : vm.pane_slots.front().pane;
    };
    const auto find_slot = [](const ui::ColumnStripLayout& layout, int id) -> const ui::ColumnStripSlot* {
        for (const auto& slot : layout.columns) if (slot.id == id) return &slot;
        return nullptr;
    };
    for (float scale : {1.0f, 1.5f}) for (float width : {760.0f, 1200.0f}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(width * scale), static_cast<int>(680 * scale),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(680 * scale));
        s.compositor.RecreateTextFormats(scale);
        tab.current_path = normalized_deep;
        tab.snapshot = empty;
        tab.snapshot_path = tab.current_path;
        tab.loading = false;
        tab.column_strip = {};
        tab.column_widths_dip = {};
        tab.ClearSelection();
        SyncColumnStrips(s);
        int column_id = -1;
        for (size_t i = 0; i < tab.column_strip.ancestors.size(); ++i)
            if (tab.column_strip.ancestors[i].path == normalized_root) column_id = static_cast<int>(i);
        check(column_id >= 0, "real navigation chain contains the fixture ancestor");
        if (column_id < 0) continue;
        auto vm = BuildVm(s);
        auto layout = s.renderer.ColumnStripGeometry(frame_pane(vm), frame_bounds(vm));
        float prefix = 0;
        for (int i = 0; i < column_id; ++i)
            prefix += ui::ColumnStripWidthDip(tab.column_widths_dip, tab.column_strip.ancestors.size() - i) * scale;
        tab.column_strip.scroll_from_right_dip = std::max(0.0f, (layout.max_scroll_px - prefix) / scale);
        vm = BuildVm(s);
        layout = s.renderer.ColumnStripGeometry(frame_pane(vm), frame_bounds(vm));
        const auto* slot = find_slot(layout, column_id);
        check(slot != nullptr, "scrolled ancestor is visible in production geometry");
        if (!slot) continue;
        const float old_x = slot->rect.left;
        POINT point{static_cast<LONG>(std::max(slot->rect.left, layout.viewport.left) + 8 * scale), 0};
        ui::HitTestResult folder_hit;
        bool found = false;
        const auto window = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        for (LONG y = static_cast<LONG>(slot->rect.top); y < slot->rect.bottom; ++y) {
            const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(point.x), static_cast<float>(y));
            if (hit.region != ui::HitTestResult::ColumnStripRow || hit.sub_index != column_id) continue;
            const auto& col = tab.column_strip.ancestors[column_id];
            if ((*col.rows_snapshot)[(*col.rows)[hit.index]].name == L"Beta") {
                point.y = y; folder_hit = hit; found = true; break;
            }
        }
        check(found, "production hit test finds the ancestor folder row");
        if (!found) continue;
        const std::vector<std::wstring> sources{(root / L"ordinary.txt").wstring()};
        check(ResolveDropTarget(s, sources, point, MK_CONTROL, DROPEFFECT_COPY | DROPEFFECT_MOVE, 0) == DROPEFFECT_COPY &&
              s.dropDestDir == normalized_beta, "ancestor folder drag targets that folder, not the deepest active directory");
        check(ResolveDropTarget(s, sources, point, MK_SHIFT, DROPEFFECT_COPY | DROPEFFECT_MOVE, 0) == DROPEFFECT_MOVE &&
              s.dropDestDir == normalized_beta, "ancestor folder move resolves the same destination as copy");
        bool blank_found = false;
        for (LONG y = static_cast<LONG>(slot->rect.bottom - 24 * scale); y > slot->rect.top; --y) {
            const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(point.x), static_cast<float>(y));
            if (hit.region != ui::HitTestResult::ColumnStripBlank || hit.sub_index != column_id) continue;
            blank_found = true;
            const POINT blank{point.x, y};
            check(ResolveDropTarget(s, sources, blank, MK_CONTROL, DROPEFFECT_COPY, 0) == DROPEFFECT_NONE,
                "ancestor blank space rejects a source already in that column's directory");
            check(ResolveDropTarget(s, {deep.wstring()}, blank, MK_CONTROL, DROPEFFECT_COPY, 0) == DROPEFFECT_COPY &&
                s.dropDestDir == normalized_root, "ancestor blank drop from another directory still targets that column");
            break;
        }
        check(blank_found, "production hit test finds ancestor blank space");
        ClearDropFeedback(s);
        tab.ClearBanner(); // The fixture injects listings with its I/O worker stopped.
        Render(s);
        Sleep(250);
        Render(s);
        const auto prefix_name = L"column-" + std::to_wstring(static_cast<int>(scale * 100)) + L"-" +
            std::to_wstring(static_cast<int>(width));
        check(WriteAnimationSnapshot(s.compositor, output / (prefix_name + L"-before.png")), "capture ancestor before navigation");
        check(HandleColumnStripMouseDown(s, vm, folder_hit, point.x) && tab.current_path == normalized_beta,
            "ancestor click enters the selected branch without opening a file");
        bool geometry_logged = false;
        const auto stable_x = [&] {
            const auto next = BuildVm(s);
            const auto next_layout = s.renderer.ColumnStripGeometry(frame_pane(next), frame_bounds(next));
            const auto* parent = find_slot(next_layout, static_cast<int>(tab.column_strip.ancestors.size()) - 1);
            if (!geometry_logged || !parent || std::abs(parent->rect.left - old_x) >= .02f) {
                geometry_logged = true;
                const auto bounds = frame_bounds(next);
                std::printf("[COLUMN GEOMETRY] scale=%.2f width=%.0f buffer=%dx%d old_x=%.2f anchor=%.2f "
                    "parent=(%.2f,%.2f) viewport=(%.2f,%.2f) bounds=(%.2f,%.2f) scroll=%.2f max=%.2f offset=%.2f\n",
                    scale, width, s.compositor.Width(), s.compositor.Height(), old_x,
                    tab.column_strip.ancestor_anchor_left_dip.value_or(-9999.0f),
                    parent ? parent->rect.left : -9999.0f, parent ? parent->rect.right : -9999.0f,
                    next_layout.viewport.left, next_layout.viewport.right, bounds.left, bounds.right,
                    next_layout.scroll_px, next_layout.max_scroll_px, tab.column_strip.scroll_from_right_dip);
            }
            return parent && std::abs(parent->rect.left - old_x) < .02f;
        };
        check(stable_x() && tab.column_strip.ancestors.back().path == normalized_root,
            "branch navigation retains the clicked column's visible x");
        check(std::none_of(tab.column_strip.ancestors.begin(), tab.column_strip.ancestors.end(),
            [&](const auto& col) { return col.path.starts_with(fs::NormalizePath(alpha.wstring())); }),
            "switching branches removes columns from the abandoned branch");
        auto& parent = tab.column_strip.ancestors.back();
        parent.generation = 50;
        parent.applied_generation = 40;
        app::WorkResult result;
        result.path = normalized_root;
        result.generation = 50;
        result.snapshot = listing;
        result.cancelled = true;
        NoteColumnStripResult(s, result);
        check(parent.generation == 50 && stable_x(), "cancelled listing leaves the clicked column and anchor intact");
        result.cancelled = false;
        result.generation = 39;
        NoteColumnStripResult(s, result);
        check(parent.generation == 50 && stable_x(), "stale listing cannot replace the retained column");
        result.generation = 50;
        result.final = true;
        result.snapshot = std::make_shared<const std::vector<fs::DirEntry>>(*listing);
        NoteColumnStripResult(s, result);
        SyncColumnStrips(s);
        check(parent.generation == 0 && stable_x(), "asynchronous accepted listing preserves the navigation anchor");
        result.path = fs::NormalizePath((alpha / L"One").wstring());
        result.generation = 500;
        NoteColumnStripResult(s, result);
        check(tab.current_path == normalized_beta && parent.path == normalized_root && stable_x(),
            "a late result for the abandoned branch cannot restore its columns or move the anchor");
        auto beta_listing = std::make_shared<std::vector<fs::DirEntry>>(1);
        beta_listing->front().name = L"Child";
        beta_listing->front().is_dir = true;
        beta_listing->front().attrs = FILE_ATTRIBUTE_DIRECTORY;
        tab.SetSnapshot(beta_listing);
        tab.snapshot_path = normalized_beta;
        tab.SelectOnly(0);
        SyncColumnStrips(s);
        check(stable_x() && tab.column_strip.child.path == fs::NormalizePath((beta / L"Child").wstring()),
            "a selected folder arriving with the current listing adds its child without shifting the clicked column");
        check(ColumnStripDropDirectory(tab, ui::kColumnStripChildId) == tab.column_strip.child.path &&
              ColumnStripDropDirectory(tab, static_cast<int>(tab.column_strip.ancestors.size()) - 1, 999).empty(),
            "child blank drops use the child folder and invalid row hits have no destination");
        tab.column_widths_dip[1] += 24;
        check(stable_x(), "resizing the clicked parent preserves its anchored left edge");
        tab.ClearBanner();
        Render(s);
        Sleep(250);
        Render(s);
        check(WriteAnimationSnapshot(s.compositor, output / (prefix_name + L"-after.png")), "capture ancestor after navigation");
        const auto& retained = tab.column_strip.ancestors.back();
        int file_row = -1;
        for (size_t i = 0; i < retained.rows->size(); ++i)
            if ((*retained.rows_snapshot)[(*retained.rows)[i]].name == L"ordinary.txt") file_row = static_cast<int>(i);
        vm = BuildVm(s);
        folder_hit.index = file_row;
        folder_hit.sub_index = static_cast<int>(tab.column_strip.ancestors.size()) - 1;
        check(file_row >= 0 && HandleColumnStripMouseDown(s, vm, folder_hit, point.x) &&
              tab.current_path == normalized_root && tab.pending_selected_name == L"ordinary.txt",
            "file click keeps the original containing-folder selection behavior");
        check(ColumnStripDropDirectory(tab, -1).empty() && ColumnStripDropDirectory(tab, 9999).empty(),
            "invalid column identifiers never fall back to the active directory");
    }
    return failures == 0;
}
