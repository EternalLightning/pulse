// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "app_column_view.h"
#include "../ui/column_strip_layout.h"
#include <algorithm>
#include <cwchar>

namespace pulse {

namespace {

constexpr size_t kMaxAncestors = 12; // bounded by the snapshot store's LRU

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

bool Eligible(const app::Tab& tab) {
    return tab.column_layout && !tab.current_path.empty() &&
           !fs::IsVirtualPath(tab.current_path) && !tab.content_results &&
           !tab.search_entries;
}

// Folder the column to the right of the list shows, or empty.
std::wstring SelectedFolder(const app::Tab& tab) {
    if (tab.SelectedCount() != 1 || tab.selected_index < 0 ||
        tab.selected_index >= static_cast<int>(tab.EntryCount())) {
        return {};
    }
    const fs::DirEntry entry = tab.EntryAt(static_cast<size_t>(tab.selected_index));
    if (entry.change_record_only) return {};
    if (entry.is_dir) return fs::NormalizePath(Join(tab.current_path, entry.name));
    if (entry.link_target_is_dir && !entry.link_target.empty())
        return fs::NormalizePath(entry.link_target);
    return {};
}

void RequestColumn(AppState& s, const app::Tab& tab, app::ColumnStripColumn& col) {
    col.requested = true;
    col.error = false;
    // A pane already loading this folder owns the refresh. Do not consume the
    // store's dirty marker: its completion still needs to see a watch retry.
    uint64_t shared = 0;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* other = pane.ActiveTab();
        if (shared == 0 && other && other->pending_generation != 0 &&
            other->sort_column == tab.sort_column && other->sort_direction == tab.sort_direction &&
            SamePath(other->current_path, col.path)) {
            shared = other->pending_generation;
        }
    });
    if (shared != 0) {
        if (!col.snapshot) col.snapshot = s.store.Peek(col.path);
        col.generation = shared;
        return;
    }
    uint64_t gen = 0;
    if (fs::SnapshotPtr fresh = s.store.GetOrStart(col.path, gen)) {
        col.snapshot = std::move(fresh);
        col.generation = 0;
        col.applied_generation = (std::max)(col.applied_generation, gen);
        return;
    }
    if (!col.snapshot) col.snapshot = s.store.Peek(col.path);
    col.generation = s.worker.Refresh(col.path, tab.sort_column, tab.sort_direction,
                                     app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode));
    if (!col.generation) {
        col.applied_generation = (std::max)(col.applied_generation, s.worker.ResultWatermark());
        col.error = true;
    }
}

void RebuildRows(app::ColumnStripColumn& col, const app::Tab& tab, app::FolderSortMode folders) {
    if (col.rows && col.rows_snapshot == col.snapshot &&
        col.rows_show_hidden == tab.show_hidden_files &&
        col.rows_show_protected == tab.show_protected_os_files &&
        col.rows_sort_column == tab.sort_column && col.rows_sort_direction == tab.sort_direction &&
        col.rows_folder_sort == folders) return;
    col.scroll_motion.Reset();
    col.rows_snapshot = col.snapshot;
    col.rows_show_hidden = tab.show_hidden_files;
    col.rows_show_protected = tab.show_protected_os_files;
    col.rows_sort_column = tab.sort_column;
    col.rows_sort_direction = tab.sort_direction;
    col.rows_folder_sort = folders;
    col.highlight_row = -1;
    if (!col.snapshot) {
        col.rows.reset();
        return;
    }
    auto rows = std::make_shared<std::vector<int>>();
    rows->reserve(col.snapshot->size());
    const auto& entries = *col.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        const fs::DirEntry& e = entries[i];
        if (e.change_record_only) continue;
        if (!tab.AllowsAttributes(e.attrs)) continue;
        rows->push_back(static_cast<int>(i));
    }
    std::sort(rows->begin(), rows->end(), [&](int a, int b) {
        return app::EntryLess(entries[static_cast<size_t>(a)], entries[static_cast<size_t>(b)],
            tab.sort_column, tab.sort_direction, folders, col.path);
    });
    for (size_t i = 0; i < rows->size(); ++i)
        if (!col.highlight_name.empty() && SamePath(entries[static_cast<size_t>((*rows)[i])].name,
                                                   col.highlight_name)) {
            col.highlight_row = static_cast<int>(i);
            break;
        }
    col.rows = std::move(rows);
}

void SyncColumn(AppState& s, const app::Tab& tab, app::ColumnStripColumn& col) {
    if (col.path.empty()) return;
    if (!col.requested) RequestColumn(s, tab, col);
    RebuildRows(col, tab, app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode));
}

// Drive roots show their sidebar label ("Data (D:)") instead of "D:".
std::wstring RootTitle(const AppState& s, const std::wstring& path) {
    for (const auto& drive : s.sidebar.drives) {
        if (SamePath(fs::NormalizePath(drive.path), path)) return drive.label;
    }
    return {};
}

void RebuildAncestors(AppState& s, app::Tab& tab) {
    std::vector<std::wstring> chain; // parent first
    std::wstring child = tab.current_path;
    while (chain.size() < kMaxAncestors) {
        std::wstring parent = fs::ParentPath(child);
        if (parent.empty() || SamePath(parent, child)) break;
        chain.push_back(parent);
        child = std::move(parent);
    }
    std::vector<app::ColumnStripColumn> next;
    next.reserve(chain.size());
    for (size_t i = chain.size(); i-- > 0;) {
        const std::wstring path = fs::NormalizePath(chain[i]);
        const std::wstring highlight = Leaf(i == 0 ? tab.current_path : chain[i - 1]);
        app::ColumnStripColumn col;
        for (auto& old : tab.column_strip.ancestors) {
            if (SamePath(old.path, path)) {
                col = std::move(old);
                break;
            }
        }
        if (col.path.empty()) col.path = path;
        if (i + 1 == chain.size()) {
            const std::wstring up = fs::ParentPath(path);
            if (up.empty() || SamePath(up, path)) col.title = RootTitle(s, path);
        }
        if (!SamePath(col.highlight_name, highlight)) {
            col.highlight_name = highlight;
            col.rows.reset(); // re-resolve the highlight row
            col.auto_scroll = true;
        }
        col.scroll_motion.Reset();
        next.push_back(std::move(col));
    }
    tab.column_strip.ancestors = std::move(next);
    // Listings may be stale after navigation; revalidate against the store.
    for (auto& col : tab.column_strip.ancestors) col.requested = false;
    tab.column_strip.for_path = tab.current_path;
    tab.column_strip.scroll_from_right_dip = 0.0f;
    tab.column_strip.horizontal_motion.Reset();
}

void SyncTab(AppState& s, app::Tab& tab) {
    app::ColumnStripState& state = tab.column_strip;
    if (!Eligible(tab)) {
        if (!state.Empty()) state = {};
        return;
    }
    if (state.for_path != tab.current_path) RebuildAncestors(s, tab);
    for (auto& col : state.ancestors) SyncColumn(s, tab, col);

    const std::wstring folder = SelectedFolder(tab);
    if (!SamePath(folder, state.child.path)) {
        state.child = {};
        state.child.path = folder;
    }
    SyncColumn(s, tab, state.child);
}

app::Tab* TabAtSlot(AppState& s, int pane_index) {
    app::Pane* pane = PaneAtSlot(s, pane_index);
    return pane ? pane->ActiveTab() : nullptr;
}

app::ColumnStripColumn* StripColumn(app::Tab& tab, int id) {
    if (id == ui::kColumnStripChildId) {
        return tab.column_strip.child.path.empty() ? nullptr : &tab.column_strip.child;
    }
    if (id < 0 || static_cast<size_t>(id) >= tab.column_strip.ancestors.size()) return nullptr;
    return &tab.column_strip.ancestors[static_cast<size_t>(id)];
}

const D2D1_RECT_F* SlotRect(const ui::WindowViewModel& vm, int pane_index) {
    if (pane_index < 0 || pane_index >= static_cast<int>(vm.pane_slots.size())) return nullptr;
    return &vm.pane_slots[static_cast<size_t>(pane_index)].rect;
}

const ui::PaneViewModel* SlotPane(const ui::WindowViewModel& vm, int pane_index) {
    if (pane_index >= 0 && pane_index < static_cast<int>(vm.pane_slots.size()))
        return &vm.pane_slots[static_cast<size_t>(pane_index)].pane;
    return pane_index <= 0 ? &vm.pane : nullptr;
}

bool SlotLayout(AppState& s, const ui::WindowViewModel& vm, int pane_index,
                ui::ColumnStripLayout& out) {
    const ui::PaneViewModel* pane = SlotPane(vm, pane_index);
    if (!pane) return false;
    const D2D1_RECT_F* rect = SlotRect(vm, pane_index);
    const D2D1_RECT_F bounds = rect ? *rect
        : s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                 static_cast<float>(s.compositor.Height()));
    out = s.renderer.ColumnStripGeometry(*pane, bounds);
    return out.active;
}

bool ScrollStripBy(AppState& s, const ui::WindowViewModel& vm, int pane_index, float delta_dip) {
    app::Tab* tab = TabAtSlot(s, pane_index);
    ui::ColumnStripLayout layout;
    if (!tab || !SlotLayout(s, vm, pane_index, layout)) return false;
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    float& offset = tab->column_strip.scroll_from_right_dip;
    tab->column_strip.horizontal_motion.ScrollBy(offset, delta_dip,
        layout.max_scroll_px / scale, (layout.viewport.right - layout.viewport.left) / scale,
        GetTickCount64(), s.renderer.MotionEnabled() && !s.shot_high_contrast && !ui::IsHighContrast());
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

// Selects `name` in the active tab now (when listed) and after the pending
// refresh lands.
void SelectByName(AppState& s, app::Tab& tab, const std::wstring& name) {
    tab.pending_selected_names = {name};
    tab.pending_selected_name = name;
    tab.pending_ensure_selection_visible = true;
    if (!tab.snapshot) return;
    const auto& entries = *tab.snapshot;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!SamePath(entries[i].name, name) || !tab.EntryVisible(static_cast<int>(i))) continue;
        tab.SelectOnly(static_cast<int>(i));
        EnsureRowVisible(s, tab, static_cast<int>(i));
        s.scrollTargetY = tab.scroll_y;
        s.scrollAnimating = false;
        break;
    }
}

bool ClickRow(AppState& s, app::Tab& tab, int column_id, int row) {
    app::ColumnStripColumn* col = StripColumn(tab, column_id);
    if (!col || !col->rows || !col->rows_snapshot || row < 0 ||
        static_cast<size_t>(row) >= col->rows->size()) {
        return false;
    }
    const int source = (*col->rows)[static_cast<size_t>(row)];
    if (source < 0 || static_cast<size_t>(source) >= col->rows_snapshot->size()) return false;
    const std::wstring name = (*col->rows_snapshot)[static_cast<size_t>(source)].name;
    const std::wstring folder = col->path; // col is invalidated by navigation
    if (!SamePath(folder, tab.current_path)) NavigateTo(s, folder,
        column_id == ui::kColumnStripChildId ? ui::SurfaceMotion::Forward : ui::SurfaceMotion::Backward);
    if (app::Tab* active = ActiveTab(s)) SelectByName(s, *active, name);
    s.stripClickTick = GetTickCount64();
    return true;
}

} // namespace

void SyncColumnStrips(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        if (app::Tab* tab = pane.ActiveTab()) SyncTab(s, *tab);
    });
}

bool TickColumnStripScroll(AppState& s, ui::WindowViewModel& vm, uint64_t now) {
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    const bool animate = s.renderer.MotionEnabled() && !s.shot_high_contrast && !ui::IsHighContrast();
    bool pending = false;
    const auto tick_pane = [&](app::Tab& tab, ui::PaneViewModel& pane,
                               const D2D1_RECT_F& bounds, int pane_index) {
        auto& strip = tab.column_strip;
        const auto layout = s.renderer.ColumnStripGeometry(pane, bounds);
        if (!Eligible(tab) || strip.for_path != tab.current_path || !layout.active) {
            strip.horizontal_motion.Reset();
            for (auto& col : strip.ancestors) col.scroll_motion.Reset();
            strip.child.scroll_motion.Reset();
            return;
        }
        if (s.stripHScrolling && s.stripResizePane == pane_index)
            strip.horizontal_motion.Reset();
        strip.horizontal_motion.Tick(strip.scroll_from_right_dip,
            layout.max_scroll_px / scale, now, animate);
        pending |= strip.horizontal_motion.Pending();
        pane.column_strip.scroll_from_right_dip = strip.scroll_from_right_dip;
        // Ancestors outside the horizontal viewport have the same row height
        // and clipping bounds, so their vertical motion can finish normally.
        const float view_h = layout.columns.empty() ? 0.0f
            : s.renderer.ColumnStripViewHeight(layout.columns.front().rect);
        const auto tick_column = [&](app::ColumnStripColumn& col, ui::ColumnStripColumnView& view) {
            if (col.auto_scroll) {
                col.scroll_motion.Reset();
                col.scroll_dip = s.renderer.ColumnStripScrollPx(view, view_h) / scale;
            } else {
                col.scroll_motion.Tick(col.scroll_dip,
                    s.renderer.ColumnStripMaxScrollPx(view, view_h) / scale, now, animate);
                pending |= col.scroll_motion.Pending();
            }
            view.scroll_dip = col.scroll_dip;
            view.auto_scroll = col.auto_scroll;
        };
        for (size_t i = 0; i < strip.ancestors.size() && i < pane.column_strip.ancestors.size(); ++i)
            tick_column(strip.ancestors[i], pane.column_strip.ancestors[i]);
        if (pane.column_strip.has_child) tick_column(strip.child, pane.column_strip.child);
    };
    if (vm.pane_slots.empty()) {
        if (app::Tab* tab = ActiveTab(s)) {
            const auto bounds = s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                                       static_cast<float>(s.compositor.Height()));
            tick_pane(*tab, vm.pane, bounds, 0);
        }
    } else {
        for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
            auto& slot = vm.pane_slots[i];
            if (app::Tab* tab = TabAtSlot(s, static_cast<int>(i))) {
                tick_pane(*tab, slot.pane, slot.rect, static_cast<int>(i));
                if (slot.focused) vm.pane.column_strip = slot.pane.column_strip;
            }
        }
    }
    return pending;
}

bool TickColumnStripScroll(AppState& s, uint64_t now) {
    auto vm = BuildVm(s, false);
    return TickColumnStripScroll(s, vm, now);
}

uint64_t SharedColumnStripGeneration(AppState& s, const std::wstring& path,
                                    const app::Tab* excluding, uint64_t generation) {
    uint64_t shared = 0;
    const auto consider = [&](const app::ColumnStripColumn& col) {
        if (col.requested && col.generation && SamePath(col.path, path) &&
            (!generation || col.generation == generation))
            shared = (std::max)(shared, col.generation);
    };
    ForEachPane(s, [&](app::Pane& pane) {
        const auto* tab = pane.ActiveTab();
        if (!tab || tab == excluding || !Eligible(*tab) ||
            tab->column_strip.for_path != tab->current_path) return;
        if (!generation && excluding && (tab->sort_column != excluding->sort_column ||
            tab->sort_direction != excluding->sort_direction)) return;
        for (const auto& col : tab->column_strip.ancestors) consider(col);
        consider(tab->column_strip.child);
    });
    return shared;
}

void InvalidateColumnStripPath(AppState& s, const std::wstring& path, fs::SnapshotPtr fresh) {
    const auto update = [&](app::ColumnStripColumn& col) {
        if (!SamePath(col.path, path)) return;
        col.scroll_motion.Reset();
        col.rows.reset();
        if (fresh) {
            col.notify_watermark = (std::max)({col.notify_watermark, col.applied_generation,
                col.generation, s.worker.ResultWatermark()});
            col.snapshot = fresh;
            col.generation = 0;
            col.requested = true;
            col.error = false;
        } else {
            col.requested = false;
        }
    };
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        auto& strip = tab->column_strip;
        for (auto& col : strip.ancestors) update(col);
        update(strip.child);
    });
}

void NoteColumnStripResult(AppState& s, const app::WorkResult& res) {
    if (res.cancelled || fs::IsVirtualPath(res.path)) return;
    bool changed = false;
    auto apply = [&](app::ColumnStripColumn& col) {
        if (col.path.empty() || !SamePath(col.path, res.path)) return;
        if (res.generation <= col.notify_watermark ||
            res.generation < col.applied_generation) return;
        if (!res.final && col.generation != res.generation) return;
        if (res.error) {
            if (col.generation == 0 || res.generation < col.generation) return;
            col.generation = 0;
            col.applied_generation = res.generation;
            col.error = true;
            changed = true;
            return;
        }
        if (!res.snapshot) return;
        if (col.generation != 0 && res.generation < col.generation) return;
        if (res.final) col.generation = 0;
        col.applied_generation = res.generation;
        col.error = false;
        col.snapshot = res.snapshot;
        changed = true;
    };
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab || tab->column_strip.Empty()) return;
        for (auto& col : tab->column_strip.ancestors) apply(col);
        apply(tab->column_strip.child);
    });
    if (changed && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleColumnStripMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x) {
    using R = ui::HitTestResult;
    switch (hit.region) {
    case R::PaneColumnLayout:
        ToggleColumnLayout(s, hit.index);
        return true;
    case R::ColumnStripDivider: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        if (!tab) return true;
        const size_t slot = ui::ColumnStripWidthSlot(hit.sub_index,
                                                     tab->column_strip.ancestors.size());
        s.stripResizing = true;
        s.stripResizePane = hit.pane_index;
        s.stripResizeColumn = hit.sub_index;
        s.stripResizeStartX = x;
        s.stripResizeStartDip = ui::ColumnStripWidthDip(tab->column_widths_dip, slot);
        s.hoverRegion = static_cast<int>(R::ColumnStripDivider);
        s.hoverSubIndex = hit.sub_index;
        s.hoverPaneIndex = hit.pane_index;
        SetCapture(s.hwnd);
        return true;
    }
    case R::ColumnStripRow: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        if (tab) ClickRow(s, *tab, hit.sub_index, hit.index);
        return true;
    }
    case R::ColumnStripBlank:
        return true;
    case R::ColumnStripHScroll: {
        app::Tab* tab = TabAtSlot(s, hit.pane_index);
        ui::ColumnStripLayout layout;
        D2D1_RECT_F track{}, thumb{};
        if (!tab || !SlotLayout(s, vm, hit.pane_index, layout) ||
            !s.renderer.ColumnStripHScrollRects(layout, track, thumb)) {
            return true;
        }
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float fx = static_cast<float>(x);
        if (fx < thumb.left || fx >= thumb.right) {
            // Page toward the click, like a classic scrollbar track.
            const float page = (layout.viewport.right - layout.viewport.left) / scale;
            ScrollStripBy(s, vm, hit.pane_index, fx < thumb.left ? page : -page);
            return true;
        }
        const float travel = (track.right - track.left) - (thumb.right - thumb.left);
        tab->column_strip.horizontal_motion.Reset();
        s.stripHScrolling = true;
        s.stripResizePane = hit.pane_index;
        s.stripResizeStartX = x;
        s.stripHScrollStartDip = tab->column_strip.scroll_from_right_dip;
        s.stripHScrollRatio = travel > 0.0f ? layout.max_scroll_px / travel : 0.0f;
        s.stripHScrollMaxDip = layout.max_scroll_px / scale;
        SetCapture(s.hwnd);
        return true;
    }
    default:
        return false;
    }
}

bool UpdateColumnStripResize(AppState& s, int x) {
    if (s.stripHScrolling) {
        app::Tab* tab = TabAtSlot(s, s.stripResizePane);
        if (!tab) return true;
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float moved = static_cast<float>(x - s.stripResizeStartX) * s.stripHScrollRatio / scale;
        tab->column_strip.horizontal_motion.Reset();
        tab->column_strip.scroll_from_right_dip =
            std::clamp(s.stripHScrollStartDip - moved, 0.0f, s.stripHScrollMaxDip);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    if (!s.stripResizing) return false;
    app::Tab* tab = TabAtSlot(s, s.stripResizePane);
    if (!tab) return true;
    const size_t slot = ui::ColumnStripWidthSlot(s.stripResizeColumn,
                                                 tab->column_strip.ancestors.size());
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    float dx = static_cast<float>(x - s.stripResizeStartX) / scale;
    // The child column's divider is its left edge: dragging left widens it.
    if (s.stripResizeColumn == ui::kColumnStripChildId) dx = -dx;
    const float width = std::clamp(s.stripResizeStartDip + dx,
                                   ui::kColumnStripMinDip, ui::kColumnStripMaxDip);
    if (tab->column_widths_dip.size() <= slot) tab->column_widths_dip.resize(slot + 1, 0.0f);
    tab->column_widths_dip[slot] = width;
    s.hoverRegion = static_cast<int>(ui::HitTestResult::ColumnStripDivider);
    s.hoverSubIndex = s.stripResizeColumn;
    s.hoverPaneIndex = s.stripResizePane;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void ResetColumnStripWidth(AppState& s, const ui::HitTestResult& hit) {
    EndColumnStripResize(s);
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    if (!tab) return;
    const size_t slot = ui::ColumnStripWidthSlot(hit.sub_index, tab->column_strip.ancestors.size());
    if (slot < tab->column_widths_dip.size()) tab->column_widths_dip[slot] = 0.0f;
    while (!tab->column_widths_dip.empty() && tab->column_widths_dip.back() <= 0.0f)
        tab->column_widths_dip.pop_back();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void EndColumnStripResize(AppState& s) {
    if (!s.stripResizing && !s.stripHScrolling) return;
    s.stripResizing = false;
    s.stripHScrolling = false;
    s.stripResizePane = -1;
    s.stripResizeColumn = -1;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool ColumnStripSwallowDoubleClick(const AppState& s) {
    return s.stripClickTick != 0 &&
           GetTickCount64() - s.stripClickTick <= static_cast<ULONGLONG>(GetDoubleClickTime());
}

bool HandleColumnStripWheel(AppState& s, const ui::WindowViewModel& vm,
                            const ui::HitTestResult& hit, int wheel_delta, bool horizontal_wheel) {
    using R = ui::HitTestResult;
    const bool on_column = hit.region == R::ColumnStripHScroll ||
        hit.region == R::ColumnStripRow || hit.region == R::ColumnStripBlank;
    const bool horizontal = (horizontal_wheel && on_column) || hit.region == R::ColumnStripHScroll ||
        ((GetKeyState(VK_SHIFT) & 0x8000) != 0 &&
         hit.sub_index != ui::kColumnStripChildId &&
         (hit.region == R::ColumnStripRow || hit.region == R::ColumnStripBlank));
    if (horizontal) {
        // Wheel up reveals farther ancestors (to the left).
        ScrollStripBy(s, vm, hit.pane_index,
                      static_cast<float>(horizontal_wheel ? -wheel_delta : wheel_delta) / WHEEL_DELTA * 96.0f);
        return true;
    }
    if (hit.region != R::ColumnStripRow && hit.region != R::ColumnStripBlank) return false;
    app::Tab* tab = TabAtSlot(s, hit.pane_index);
    const D2D1_RECT_F* rect = SlotRect(vm, hit.pane_index);
    const ui::PaneViewModel* pane = SlotPane(vm, hit.pane_index);
    if (!tab || !pane) return true;
    D2D1_RECT_F bounds = rect ? *rect
        : s.renderer.ContentRect(static_cast<float>(s.compositor.Width()),
                                 static_cast<float>(s.compositor.Height()));
    const ui::ColumnStripLayout layout = s.renderer.ColumnStripGeometry(*pane, bounds);
    app::ColumnStripColumn* col = StripColumn(*tab, hit.sub_index);
    if (!col) return true;
    const ui::ColumnStripColumnView* view = nullptr;
    if (hit.sub_index == ui::kColumnStripChildId) view = &pane->column_strip.child;
    else if (hit.sub_index >= 0 &&
             static_cast<size_t>(hit.sub_index) < pane->column_strip.ancestors.size())
        view = &pane->column_strip.ancestors[static_cast<size_t>(hit.sub_index)];
    if (!view) return true;
    for (const auto& slot : layout.columns) {
        if (slot.id != hit.sub_index) continue;
        const float view_h = s.renderer.ColumnStripViewHeight(slot.rect);
        const float scale = s.scale > 0.0f ? s.scale : 1.0f;
        const float current = s.renderer.ColumnStripScrollPx(*view, view_h);
        const float max_scroll = s.renderer.ColumnStripMaxScrollPx(*view, view_h);
        const float step = static_cast<float>(wheel_delta) / WHEEL_DELTA * 3.0f *
                           s.renderer.RowHeight();
        if (col->auto_scroll) {
            col->scroll_motion.Reset();
            col->scroll_dip = current / scale;
        }
        col->auto_scroll = false;
        col->scroll_motion.ScrollBy(col->scroll_dip, -step / scale, max_scroll / scale,
            view_h / scale, GetTickCount64(),
            s.renderer.MotionEnabled() && !s.shot_high_contrast && !ui::IsHighContrast());
        InvalidateRect(s.hwnd, nullptr, FALSE);
        break;
    }
    return true;
}

bool HandleColumnArrowKey(AppState& s, bool right) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !Eligible(*tab)) return false;
    if (tab->view_mode != ui::ViewMode::Details && tab->view_mode != ui::ViewMode::Content)
        return false;
    if (!right) {
        GoUp(s);
        return true;
    }
    const std::wstring folder = SelectedFolder(*tab);
    if (folder.empty()) return false;
    NavigateTo(s, folder, ui::SurfaceMotion::Forward);
    return true;
}

void ToggleColumnLayout(AppState& s, int pane_index) {
    app::Pane* pane = PaneAtSlot(s, pane_index);
    if (!pane) pane = s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab) return;
    tab->column_layout = !tab->column_layout;
    if (!tab->column_layout) {
        tab->column_strip = {};
    } else if (!s.showDetailsPanel) {
        // Preview and tags of the selected file live in the details panel.
        s.showDetailsPanel = true;
        s.renderer.SetDetailsPanelVisible(true);
    }
    SyncColumnStrips(s);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
