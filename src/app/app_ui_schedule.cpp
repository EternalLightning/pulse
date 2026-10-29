#include "app_ui_schedule.h"
#include "app_internal.h"

namespace pulse {

void StopUiScheduling(AppState& s) {
    KillTimer(s.hwnd, kTimerUi);
    KillTimer(s.hwnd, kTimerUiDeadline);
    KillTimer(s.hwnd, kTimerDeferredStartup);
    s.ui_schedule.stopped = true;
    s.ui_schedule.options_due = 0;
    s.ui_schedule.deadline = 0;
    s.ui_schedule.maintenance_ms = 0;
}

void SyncUiScheduling(AppState& s) {
    if (s.ui_schedule.stopped || !s.hwnd || !IsWindow(s.hwnd)) return;
    const bool visible = IsWindowVisible(s.hwnd) && !IsIconic(s.hwnd);
    const uint64_t now = GetTickCount64();
    if (!s.ui_schedule.last_activity) s.ui_schedule.NoteActivity(now);
    const UINT interval = s.ui_schedule.MaintenanceInterval(visible, s.exit_requested, now);
    if (s.ui_schedule.maintenance_ms != interval && SetTimer(s.hwnd, kTimerUi, interval, nullptr))
        s.ui_schedule.maintenance_ms = interval;

    const bool options_ready = !s.exit_requested && visible && s.searchOptionsPending &&
        !s.searchHistoryOpen && (!s.menu || !s.menu->IsOpen());
    if (!options_ready) s.ui_schedule.options_due = 0;
    else if (!s.ui_schedule.options_due) s.ui_schedule.options_due = now ? now : 1;
    app::UiDeadlinePlan plan;
    plan.Include(s.ui_schedule.options_due);
    if (!s.exit_requested) {
        if (!s.addressSearchComposing) plan.Include(s.addressLiveDue);
        plan.Include(s.addressHistoryDue);
        if (s.renameClickCandidate) plan.Include(s.renameClickDue);
        plan.Include(s.context_menu.NextRefreshDeadline());
        if (visible) {
            if (s.hoverRegion && s.tooltipText.empty() && s.hoverSince &&
                s.ui_schedule.tooltip_checked_since != s.hoverSince)
                plan.Include(s.hoverSince + 150);
            plan.Include(s.renderer.DetailsPreviewDeadline());
            plan.Include(s.sidebarFadeDeadline);
        }
    }
    if (plan.due == s.ui_schedule.deadline) return;
    KillTimer(s.hwnd, kTimerUiDeadline);
    s.ui_schedule.deadline = 0;
    if (plan.due && SetTimer(s.hwnd, kTimerUiDeadline, plan.Delay(now), nullptr))
        s.ui_schedule.deadline = plan.due;
}

bool RunUiDeadlines(AppState& s, uint64_t now) {
    bool dirty = TickAddressSearchDeadlines(s, now);
    const int refreshes = s.context_menu.ConsumeDueRefreshes(now);
    for (int i = 0; i < refreshes; ++i) {
        RefreshActiveTab(s, RefreshReason::ShellNotification);
        dirty = true;
    }
    if (s.renameClickCandidate && s.renameClickDue && now >= s.renameClickDue) {
        app::Tab* tab = ActiveTab(s);
        const int index = s.renameClickIndex;
        const bool valid = s.pane == s.renameClickPane && tab == s.renameClickTab && tab &&
            tab->SelectedCount() == 1 && tab->selected_index == index &&
            index >= 0 && index < tab->CountBound() &&
            (s.renameClickPath.empty() || EntryFullPath(*tab, index) == s.renameClickPath) &&
            s.renameIndex < 0 && s.tagRenameId.empty() && !s.addressEditing && !s.filterEditing &&
            !s.dragPending && !s.marqueePending && !s.marqueeActive &&
            !s.scrollbarDragging && !s.splitterDragging;
        CancelRenameClick(s);
        if (valid) ShowRenameOverlay(s);
    }
    if (IsWindowVisible(s.hwnd) && !IsIconic(s.hwnd)) {
        if (s.hoverRegion && s.tooltipText.empty() && s.hoverSince && now >= s.hoverSince + 150 &&
            s.ui_schedule.tooltip_checked_since != s.hoverSince) {
            s.ui_schedule.tooltip_checked_since = s.hoverSince;
            s.tooltipText = TooltipForHover(s);
            dirty |= !s.tooltipText.empty();
        }
        dirty |= s.renderer.TickDetailsPreview(now);
        const uint64_t visual_due = s.sidebarFadeDeadline;
        if (visual_due && visual_due <= now) {
            s.sidebarFadeDeadline = 0;
            dirty = true;
        }
    }
    return dirty;
}

void CompleteFilterFocus(AppState& s) {
    if (!s.filterFocusPending || !s.pane || s.pane->filter_expand < 1 || !s.hwndFilterEdit) return;
    LayoutFilterEditor(s);
    s.filterIgnoreKillFocus = true;
    ShowWindow(s.hwndFilterEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndFilterEdit, GA_ROOT));
    SetFocus(s.hwndFilterEdit);
    SendMessageW(s.hwndFilterEdit, EM_SETSEL, 0, -1);
    s.filterFocusPending = false;
    s.filterIgnoreKillFocus = false;
}

} // namespace pulse
