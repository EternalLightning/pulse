#include "tab_transfer.h"
#include "app_runtime.h"
#include "app_internal.h"

namespace pulse {
namespace {
void CancelTransferredPaneWork(AppState& source, app::Pane& pane) {
    app::Tab& tab = pane.view;
    if (!tab.current_path.empty() && tab.pending_generation)
        source.worker.Cancel(tab.current_path, tab.pending_generation);
    source.index.CancelSession(tab.search_session_id);
    source.networkIndex.CancelSession(tab.search_session_id);
    if (tab.archive_cancel) tab.archive_cancel->store(true);
}

void ReloadTransferredLayout(AppState& destination, app::LayoutTab& layout) {
    for (auto& pane : layout.panes) {
        if (!pane || pane->view.current_path.empty()) continue;
        const std::wstring path = pane->view.current_path;
        StartLoadingPath(destination, pane->view, path, PathLoadReason::Navigate);
    }
}

app::LayoutPreset PresetForPaneCount(size_t count) {
    if (count <= 1) return app::LayoutPreset::Single;
    if (count == 2) return app::LayoutPreset::TwoVertical;
    if (count == 3) return app::LayoutPreset::Three;
    return app::LayoutPreset::FourGrid;
}

void TransferTabGroupMembership(app::WindowTabs& source, app::WindowTabs& destination,
                                app::LayoutTab& tab) {
    if (tab.tab_group == 0) return;
    const auto group = std::find_if(source.tab_groups.begin(), source.tab_groups.end(),
        [&](const app::TabGroup& candidate) { return candidate.id == tab.tab_group; });
    if (group == source.tab_groups.end()) {
        tab.tab_group = 0;
        return;
    }
    auto matching = std::find_if(destination.tab_groups.begin(), destination.tab_groups.end(),
        [&](const app::TabGroup& candidate) {
            return candidate.name == group->name && candidate.color_rgb == group->color_rgb;
        });
    if (matching != destination.tab_groups.end()) {
        tab.tab_group = matching->id;
        return;
    }
    int next_id = std::max(1, destination.next_tab_group_id);
    for (const auto& candidate : destination.tab_groups)
        next_id = std::max(next_id, candidate.id + 1);
    destination.tab_groups.push_back({next_id, group->name, group->color_rgb, group->collapsed});
    destination.next_tab_group_id = next_id + 1;
    tab.tab_group = next_id;
}
}

bool TransferLayoutTab(AppState& source, size_t index, AppState& destination,
                       bool close_empty_source, bool replace_destination_starter) {
    if (&source == &destination || index >= source.window_tabs.items.size()) return false;
    auto& src = source.window_tabs;
    auto& dst = destination.window_tabs;
    const bool moved_pinned = src.items[index]->pinned;
    auto moved = std::move(src.items[index]);
    for (auto& pane : moved->panes) if (pane) CancelTransferredPaneWork(source, *pane);
    src.items.erase(src.items.begin() + static_cast<ptrdiff_t>(index));
    const bool source_became_empty = src.items.empty();
    if (source_became_empty) {
        src.active = 0;
    } else if (src.active > index) {
        --src.active;
    } else if (src.active >= src.items.size()) {
        src.active = src.items.size() - 1;
    }
    TransferTabGroupMembership(src, dst, *moved);
    dst.EnsureDefault();
    if (replace_destination_starter) {
        for (auto& pane : dst.items.front()->panes)
            if (pane) CancelTransferredPaneWork(destination, *pane);
        dst.items.clear();
        dst.active = 0;
    }
    size_t insert_at = dst.items.size();
    if (moved_pinned) {
        insert_at = 0;
        while (insert_at < dst.items.size() && dst.items[insert_at]->pinned) ++insert_at;
    }
    dst.items.insert(dst.items.begin() + static_cast<ptrdiff_t>(insert_at), std::move(moved));
    dst.active = insert_at;
    app::NormalizeGroupRuns(dst);
    auto& groups = src.tab_groups;
    std::erase_if(groups, [&](const app::TabGroup& group) {
        return std::none_of(src.items.begin(), src.items.end(), [&](const auto& tab) {
            return tab && tab->tab_group == group.id;
        });
    });
    BindCurrentLayout(source);
    BindCurrentLayout(destination);
    ReloadTransferredLayout(destination, *dst.Active());
    if (source_became_empty && close_empty_source && source.hwnd)
        PostMessageW(source.hwnd, WM_CLOSE, 0, 0);
    InvalidateRect(source.hwnd, nullptr, FALSE);
    InvalidateRect(destination.hwnd, nullptr, FALSE);
    return true;
}

bool TransferPane(AppState& source, size_t pane_index, AppState& destination,
                 size_t source_layout_index, bool replace_destination_starter) {
    if (&source == &destination) return false;
    if (source_layout_index == static_cast<size_t>(-1))
        source_layout_index = source.window_tabs.active;
    if (source_layout_index >= source.window_tabs.items.size()) return false;
    app::LayoutTab* src = source.window_tabs.items[source_layout_index].get();
    if (src && src->panes.size() == 1)
        return TransferLayoutTab(source, source_layout_index, destination, true,
                                 replace_destination_starter);
    destination.window_tabs.EnsureDefault();
    app::LayoutTab* dst = destination.window_tabs.Active();
    if (!src || pane_index >= src->panes.size() || !dst || dst->panes.size() >= 4)
        return false;
    CancelTransferredPaneWork(source, *src->panes[pane_index]);
    auto moved = std::move(src->panes[pane_index]);
    src->panes.erase(src->panes.begin() + static_cast<ptrdiff_t>(pane_index));
    const bool source_replaced = src->panes.empty();
    if (source_replaced) src->panes.push_back(std::make_unique<app::Pane>());
    src->focused_index = std::min(src->focused_index,
        static_cast<int>(src->panes.size()) - 1);
    src->target_index = -1;
    src->layout = PresetForPaneCount(src->panes.size());
    app::RebuildLayoutRoot(*src);
    if (replace_destination_starter) {
        CancelTransferredPaneWork(destination, *dst->panes[0]);
        dst->panes[0] = std::move(moved);
        dst->focused_index = 0;
    } else {
        dst->panes.push_back(std::move(moved));
        dst->focused_index = static_cast<int>(dst->panes.size()) - 1;
        dst->layout = PresetForPaneCount(dst->panes.size());
    }
    dst->target_index = -1;
    app::RebuildLayoutRoot(*dst);
    BindCurrentLayout(source);
    BindCurrentLayout(destination);
    ReloadTransferredLayout(destination, *dst);
    if (source_replaced && source.pane)
        StartLoadingPath(source, source.pane->view, L"", PathLoadReason::Navigate);
    InvalidateRect(source.hwnd, nullptr, FALSE);
    InvalidateRect(destination.hwnd, nullptr, FALSE);
    return true;
}

bool TransferLayoutTabIntoPaneArea(AppState& source, size_t index, AppState& destination) {
    if (&source == &destination || index >= source.window_tabs.items.size()) return false;
    app::LayoutTab* incoming = source.window_tabs.items[index].get();
    if (!incoming || incoming->panes.empty()) return false;
    destination.window_tabs.EnsureDefault();
    app::LayoutTab* target = destination.window_tabs.Active();
    if (!target) return false;
    const bool replace_starter = false;
    const size_t target_count = replace_starter ? 0 : target->panes.size();
    if (target_count + incoming->panes.size() > 4)
        return TransferLayoutTab(source, index, destination);

    auto moved = std::move(source.window_tabs.items[index]);
    for (auto& pane : moved->panes) if (pane) CancelTransferredPaneWork(source, *pane);
    source.window_tabs.items.erase(source.window_tabs.items.begin() + static_cast<ptrdiff_t>(index));
    const bool source_empty = source.window_tabs.items.empty();
    if (source_empty) source.window_tabs.active = 0;
    else if (source.window_tabs.active > index) --source.window_tabs.active;
    else if (source.window_tabs.active >= source.window_tabs.items.size())
        source.window_tabs.active = source.window_tabs.items.size() - 1;

    if (replace_starter) {
        target->panes.clear();
        target->focused_index = 0;
    }
    for (auto& pane : moved->panes) target->panes.push_back(std::move(pane));
    target->focused_index = static_cast<int>(target->panes.size()) - 1;
    target->target_index = -1;
    target->layout = PresetForPaneCount(target->panes.size());
    app::RebuildLayoutRoot(*target);
    std::erase_if(source.window_tabs.tab_groups, [&](const app::TabGroup& group) {
        return std::none_of(source.window_tabs.items.begin(), source.window_tabs.items.end(),
            [&](const auto& tab) { return tab && tab->tab_group == group.id; });
    });
    BindCurrentLayout(source);
    BindCurrentLayout(destination);
    ReloadTransferredLayout(destination, *target);
    if (source_empty && source.hwnd) PostMessageW(source.hwnd, WM_CLOSE, 0, 0);
    InvalidateRect(source.hwnd, nullptr, FALSE);
    InvalidateRect(destination.hwnd, nullptr, FALSE);
    return true;
}

bool TryTransferLayoutTabAtScreenPoint(AppState& source, size_t index, POINT screen_point) {
    bool transferred = false;
    bool covered_by_window = false;
    source.core.windows.ForEach([&](HWND hwnd, AppState& target) {
        if (transferred || !target.hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr) || screen_point.x < wr.left || screen_point.x >= wr.right ||
            screen_point.y < wr.top || screen_point.y >= wr.bottom) return;
        covered_by_window = true;
        if (&target == &source) return;
        POINT client = screen_point;
        ScreenToClient(hwnd, &client);
        const D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            static_cast<float>(target.compositor.Width()),
            static_cast<float>(target.compositor.Height()));
        const ui::HitTestResult hit = target.renderer.HitTest(BuildVm(target, false), bounds,
            static_cast<float>(client.x), static_cast<float>(client.y));
        if (client.y >= 0 && client.y < static_cast<LONG>(target.renderer.TitleBarHeight())) {
            transferred = TransferLayoutTab(source, index, target);
            return;
        }
        switch (hit.region) {
        case ui::HitTestResult::Tab:
        case ui::HitTestResult::TabClose:
        case ui::HitTestResult::TabNew:
        case ui::HitTestResult::TabGroup:
        case ui::HitTestResult::TabScrollLeft:
        case ui::HitTestResult::TabScrollRight:
            transferred = TransferLayoutTab(source, index, target);
            break;
        case ui::HitTestResult::Row:
        case ui::HitTestResult::Pane:
        case ui::HitTestResult::PaneEmptyNewFolder:
            transferred = TransferLayoutTabIntoPaneArea(source, index, target);
            break;
        default: break;
        }
    });
    if (!transferred && !covered_by_window && source.hwnd) {
        transferred = PostMessageW(source.hwnd, WM_DETACH_LAYOUT_TAB,
            static_cast<WPARAM>(index), 0) != FALSE;
    }
    return transferred;
}

bool TryMergeWindowAtScreenPoint(AppState& source, POINT screen_point) {
    bool merged = false;
    source.core.windows.ForEach([&](HWND hwnd, AppState& target) {
        if (merged || &target == &source || !target.hwnd ||
            !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr) || screen_point.x < wr.left || screen_point.x >= wr.right ||
            screen_point.y < wr.top || screen_point.y >= wr.bottom) return;
        POINT client = screen_point;
        ScreenToClient(hwnd, &client);
        if (client.y > static_cast<LONG>(target.renderer.TitleBarHeight())) return;
        while (!source.window_tabs.items.empty()) {
            if (!TransferLayoutTab(source, 0, target, false)) return;
        }
        merged = true;
        if (source.hwnd) PostMessageW(source.hwnd, WM_CLOSE, 0, 0);
    });
    return merged;
}

bool TryTransferPaneAtScreenPoint(AppState& source, size_t layout_index,
                                 size_t pane_index, POINT screen_point) {
    bool transferred = false;
    bool covered_by_window = false;
    source.core.windows.ForEach([&](HWND hwnd, AppState& target) {
        if (transferred || !target.hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return;
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr) || screen_point.x < wr.left || screen_point.x >= wr.right ||
            screen_point.y < wr.top || screen_point.y >= wr.bottom) return;
        covered_by_window = true;
        if (&target == &source) return;
        POINT client = screen_point;
        ScreenToClient(hwnd, &client);
        const D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            static_cast<float>(target.compositor.Width()),
            static_cast<float>(target.compositor.Height()));
        const auto hit = target.renderer.HitTest(BuildVm(target, false), bounds,
            static_cast<float>(client.x), static_cast<float>(client.y));
        if (hit.pane_index < 0 || (hit.region != ui::HitTestResult::Row &&
            hit.region != ui::HitTestResult::Pane &&
            hit.region != ui::HitTestResult::PaneEmptyNewFolder)) return;
        transferred = TransferPane(source, pane_index, target, layout_index);
    });
    if (!transferred && !covered_by_window && source.hwnd) {
        transferred = PostMessageW(source.hwnd, WM_DETACH_PANE,
            static_cast<WPARAM>(pane_index), static_cast<LPARAM>(layout_index)) != FALSE;
    }
    return transferred;
}

} // namespace pulse
