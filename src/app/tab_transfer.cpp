#include "tab_transfer.h"
#include "app_runtime.h"
#include "app_internal.h"
#include "app_exit.h"
#include "tab_group_id.h"
#include "content_results_ui.h"
#include <dwmapi.h>

namespace pulse {
namespace {
HHOOK caption_escape_hook = nullptr;
HHOOK caption_message_hook = nullptr;
HWND caption_drag_window = nullptr;
bool caption_drag_cancelled = false;

LRESULT CALLBACK CaptionEscapeHook(int code, WPARAM key, LPARAM flags) {
    if (code == HC_ACTION && key == VK_ESCAPE && !(flags & (1LL << 31)))
        caption_drag_cancelled = true;
    return CallNextHookEx(caption_escape_hook, code, key, flags);
}

LRESULT CALLBACK CaptionMessageHook(int code, WPARAM removed, LPARAM value) {
    const auto* message = reinterpret_cast<const MSG*>(value);
    if (code >= 0 && removed == PM_REMOVE && message && message->message == WM_KEYDOWN &&
        message->wParam == VK_ESCAPE) caption_drag_cancelled = true;
    return CallNextHookEx(caption_message_hook, code, removed, value);
}

void CancelTransferredPaneWork(AppState& source, app::Pane& pane) {
    app::Tab& tab = pane.view;
    TeardownTabSearch(source, tab);
    CancelTransferredListing(source, tab);
    if (tab.archive_cancel) tab.archive_cancel->store(true);
    source.explicit_entry_refreshes.erase(&tab);
}

void RescaleTransferredPane(const AppState& source, const AppState& destination, app::Pane& pane) {
    const float ratio = source.scale > 0.0f && destination.scale > 0.0f
        ? destination.scale / source.scale : 1.0f;
    pane.view.scroll_x *= ratio;
    pane.view.scroll_y *= ratio;
    const auto scale_viewport = [&](app::Tab::NavigationViewport& viewport) {
        viewport.x *= ratio;
        viewport.y *= ratio;
    };
    if (pane.view.pending_history_viewport) scale_viewport(*pane.view.pending_history_viewport);
    const auto scale_history = [&](auto& stack) {
        std::vector<app::Tab::HistoryEntry> entries;
        while (!stack.empty()) {
            auto entry = std::move(stack.top());
            stack.pop();
            if (entry.viewport) scale_viewport(*entry.viewport);
            entries.push_back(std::move(entry));
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) stack.push(std::move(*it));
    };
    scale_history(pane.view.back_stack);
    scale_history(pane.view.forward_stack);
}

void ResumeTransferredLayout(AppState& destination, app::LayoutTab& layout) {
    for (auto& pane : layout.panes) {
        if (pane) ResumeTransferredTab(destination, pane->view);
    }
}

void ReleaseTransferredLayoutInteraction(AppState& state, const app::LayoutTab& layout) {
    const auto owns = [&](const app::Tab* view, const app::Pane* pane) {
        return std::any_of(layout.panes.begin(), layout.panes.end(), [&](const auto& item) {
            return item && (item.get() == pane || &item->view == view);
        });
    };
    if (owns(nullptr, state.pane)) {
        if (state.renameIndex >= 0) HideRenameOverlay(state, false);
        if (state.addressEditing) HideAddressEditor(state, false);
        if (state.filterEditing) HideFilterEditor(state, true);
        state.quickPreview.Close();
    }
    if (owns(state.renameClickTab, state.renameClickPane)) CancelRenameClick(state);
    if (owns(state.blankClickTab, state.blankClickPane)) {
        state.blankClickTab = nullptr;
        state.blankClickPane = nullptr;
    }
    if (owns(state.blankDoubleTab, state.blankDoublePane)) {
        state.blankDoubleTab = nullptr;
        state.blankDoublePane = nullptr;
        state.blankDoublePending = false;
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
    const int next_id = app::AllocateTabGroupId(destination);
    if (next_id == 0) {
        tab.tab_group = 0;
        return;
    }
    destination.tab_groups.push_back({next_id, group->name, group->color_rgb, group->collapsed});
    tab.tab_group = next_id;
}
}

bool IsWindowTransferDesktopSurface(HWND hwnd) {
    const HWND desktop = GetShellWindow();
    if (!hwnd || !desktop) return false;
    if (hwnd == desktop) return true;
    DWORD desktop_process = 0, window_process = 0;
    GetWindowThreadProcessId(desktop, &desktop_process);
    GetWindowThreadProcessId(hwnd, &window_process);
    if (!desktop_process || desktop_process != window_process) return false;
    wchar_t class_name[64]{};
    if (!GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name))) return false;
    // Explorer can put the wallpaper and desktop icons in separate WorkerW
    // surfaces. Other Explorer windows and foreign tool windows still block.
    return wcscmp(class_name, L"Progman") == 0 || wcscmp(class_name, L"WorkerW") == 0;
}

AppState* WindowTransferTargetAtPoint(AppState& source, POINT screen_point, bool& covered, bool ignore_source) {
    covered = false;
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        if ((ignore_source && hwnd == source.hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, screen_point)) continue;
        if (IsWindowTransferDesktopSurface(hwnd)) return nullptr;
        covered = true;
        return source.core.windows.Lookup(hwnd);
    }
    // A drop back onto the source is a reorder, never a new window.
    RECT source_rect{};
    covered = source.hwnd && IsWindowVisible(source.hwnd) &&
        GetWindowRect(source.hwnd, &source_rect) && PtInRect(&source_rect, screen_point);
    return nullptr;
}

bool CancelWindowTransferDrag(AppState& state) {
    if (!state.tabDragPending && !state.tabDragging &&
        !state.paneDragPending && !state.paneDragging) return false;
    state.paneDragPending = false;
    state.paneDragging = false;
    state.paneDragLayoutIndex = state.paneDragPaneIndex = -1;
    state.tabDragPending = false;
    state.tabDragging = false;
    state.tabDragIndex = -1;
    state.tabDragRunPos = 0;
    state.tabDragRunLen = 1;
    state.tabDragFromChip = false;
    state.tabDragGroupId = 0;
    state.tabDragSlots = 1.0f;
    state.tabDragBlockW = 0.0f;
    state.tabOrder.clear();
    state.tabTracks.clear();
    state.tabOffsets.clear();
    state.chipTracks.clear();
    state.chipOffsets.clear();
    if (GetCapture() == state.hwnd) ReleaseCapture();
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

void BeginWindowCaptionTransfer(HWND hwnd) {
    if (caption_drag_window) return;
    caption_drag_window = hwnd;
    caption_drag_cancelled = false;
    caption_escape_hook = SetWindowsHookExW(WH_KEYBOARD, CaptionEscapeHook, nullptr, GetCurrentThreadId());
    caption_message_hook = SetWindowsHookExW(WH_GETMESSAGE, CaptionMessageHook, nullptr, GetCurrentThreadId());
    // Without the hook we cannot reliably distinguish Esc in the system loop.
    if (!caption_escape_hook && !caption_message_hook) caption_drag_cancelled = true;
}

bool EndWindowCaptionTransfer(HWND hwnd) {
    if (caption_drag_window != hwnd) return false;
    const bool accepted = !caption_drag_cancelled;
    CancelWindowCaptionTransfer(hwnd);
    return accepted;
}

void CancelWindowCaptionTransfer(HWND hwnd) {
    if (caption_drag_window != hwnd) return;
    if (caption_escape_hook) UnhookWindowsHookEx(caption_escape_hook);
    if (caption_message_hook) UnhookWindowsHookEx(caption_message_hook);
    caption_escape_hook = nullptr;
    caption_message_hook = nullptr;
    caption_drag_window = nullptr;
    caption_drag_cancelled = false;
}

bool TransferLayoutTab(AppState& source, size_t index, AppState& destination,
                       bool close_empty_source, bool replace_destination_starter) {
    if (&source == &destination || index >= source.window_tabs.items.size()) return false;
    auto& src = source.window_tabs;
    auto& dst = destination.window_tabs;
    RememberLayoutFocus(source);
    RememberLayoutFocus(destination);
    ReleaseTransferredLayoutInteraction(source, *src.items[index]);
    const bool moved_pinned = src.items[index]->pinned;
    auto moved = std::move(src.items[index]);
    for (auto& pane : moved->panes) if (pane) {
        CancelTransferredPaneWork(source, *pane);
        RescaleTransferredPane(source, destination, *pane);
    }
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
    ResumeTransferredLayout(destination, *dst.Active());
    if (source_became_empty && close_empty_source && source.hwnd)
        RequestWindowClose(source);
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
    RememberLayoutFocus(source);
    RememberLayoutFocus(destination);
    ReleaseTransferredLayoutInteraction(source, *src);
    CancelTransferredPaneWork(source, *src->panes[pane_index]);
    RescaleTransferredPane(source, destination, *src->panes[pane_index]);
    auto moved = std::move(src->panes[pane_index]);
    app::Pane* transferred = moved.get();
    src->panes.erase(src->panes.begin() + static_cast<ptrdiff_t>(pane_index));
    const bool source_replaced = src->panes.empty();
    if (source_replaced) src->panes.push_back(std::make_unique<app::Pane>());
    if (src->focused_index > static_cast<int>(pane_index)) --src->focused_index;
    src->focused_index = std::min(src->focused_index, static_cast<int>(src->panes.size()) - 1);
    if (src->target_index == static_cast<int>(pane_index)) src->target_index = -1;
    else if (src->target_index > static_cast<int>(pane_index)) --src->target_index;
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
    ResumeTransferredTab(destination, transferred->view);
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

    RememberLayoutFocus(source);
    RememberLayoutFocus(destination);
    ReleaseTransferredLayoutInteraction(source, *incoming);
    auto moved = std::move(source.window_tabs.items[index]);
    std::vector<app::Pane*> transferred;
    for (auto& pane : moved->panes) if (pane) {
        CancelTransferredPaneWork(source, *pane);
        RescaleTransferredPane(source, destination, *pane);
        transferred.push_back(pane.get());
    }
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
    for (app::Pane* pane : transferred) ResumeTransferredTab(destination, pane->view);
    if (source_empty && source.hwnd) RequestWindowClose(source);
    InvalidateRect(source.hwnd, nullptr, FALSE);
    InvalidateRect(destination.hwnd, nullptr, FALSE);
    return true;
}

bool TryTransferLayoutTabAtScreenPoint(AppState& source, size_t index, POINT screen_point) {
    bool transferred = false;
    bool covered_by_window = false;
    auto route = [&](HWND hwnd, AppState& target) {
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
    };
    if (AppState* target = WindowTransferTargetAtPoint(source, screen_point, covered_by_window))
        route(target->hwnd, *target);
    if (!transferred && !covered_by_window && source.hwnd) {
        transferred = PostMessageW(source.hwnd, WM_DETACH_LAYOUT_TAB,
            static_cast<WPARAM>(index), 0) != FALSE;
    }
    return transferred;
}

bool TryMergeWindowAtScreenPoint(AppState& source, POINT screen_point) {
    bool merged = false;
    auto route = [&](HWND hwnd, AppState& target) {
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
        if (source.hwnd) RequestWindowClose(source);
    };
    bool covered = false;
    if (AppState* target = WindowTransferTargetAtPoint(source, screen_point, covered, true))
        route(target->hwnd, *target);
    return merged;
}

bool TryTransferPaneAtScreenPoint(AppState& source, size_t layout_index,
                                 size_t pane_index, POINT screen_point) {
    bool transferred = false;
    bool covered_by_window = false;
    auto route = [&](HWND hwnd, AppState& target) {
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
    };
    if (AppState* target = WindowTransferTargetAtPoint(source, screen_point, covered_by_window))
        route(target->hwnd, *target);
    if (!transferred && !covered_by_window && source.hwnd) {
        transferred = PostMessageW(source.hwnd, WM_DETACH_PANE,
            static_cast<WPARAM>(pane_index), static_cast<LPARAM>(layout_index)) != FALSE;
    }
    return transferred;
}

} // namespace pulse
