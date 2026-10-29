#include "app_internal.h"
#include "../ui/toolbar_layout.h"
#include "../common/path_utils.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse {

void UpdateOperationCenterTheme(AppState& s) {
    if (!s.operationCenter) return;
    using I = l10n::StringId;
    ui::OperationCenterLabels labels;
    labels.title = l10n::Get(I::OperationCenter);
    labels.empty = l10n::Get(I::OperationCenterEmpty);
    labels.current = l10n::Get(I::OperationCenterCurrent);
    labels.queued = l10n::Get(I::OperationCenterQueued);
    labels.recent = l10n::Get(I::OperationCenterRecent);
    labels.clear_finished = l10n::Get(I::OperationCenterClear);
    labels.details = l10n::Get(I::OpDetails);
    labels.cancel = l10n::Get(I::Cancel);
    labels.pause = l10n::Get(I::OpPause);
    labels.resume = l10n::Get(I::OpResume);
    labels.open_destination = l10n::Get(I::OperationCenterDestination);
    labels.return_to_tray = l10n::Get(I::ReturnUnfinishedToTray);
    labels.resume_incomplete = l10n::Get(I::ResumeIncompleteTransfer);
    labels.remaining_time = l10n::Get(I::OpRemainingTime);
    labels.remaining_items = l10n::Get(I::OpRemainingItems);
    labels.remaining_size = l10n::Get(I::OpRemainingSize);
    labels.current_speed = l10n::Get(I::OpCurrentSpeed);
    labels.estimating = l10n::Get(I::OpEstimating);
    labels.type_names = {l10n::Get(I::Copy), l10n::Get(I::OperationMove),
        l10n::Get(I::OperationCenterRecycle), l10n::Get(I::Delete), l10n::Get(I::Rename),
        l10n::Get(I::NewFolder), l10n::Get(I::NewTextDocument), l10n::Get(I::Restore),
        l10n::Get(I::EmptyRecycleBin), l10n::Get(I::BatchRename)};
    labels.phase_names = {l10n::Get(I::OperationCenterQueued), l10n::Get(I::OpScanning),
        l10n::Get(I::OpWaitingConflict), l10n::Get(I::OperationCenterRunning),
        l10n::Get(I::OpPaused), l10n::Get(I::OperationCenterVerifying),
        l10n::Get(I::OperationCenterCancelling), l10n::Get(I::OpCompleted),
        l10n::Get(I::OpFailed), l10n::Get(I::OperationCenterWaiting),
        l10n::Get(I::OperationCenterCancelled)};
    s.operationCenter->SetTheme(s.darkMode, s.accentColor);
    s.operationCenter->SetLabels(std::move(labels));
    if (s.operationHistoryWindow) s.operationHistoryWindow->SetTheme(s.darkMode, s.accentColor);
}

std::vector<ops::OpStatus> CollectOperationCenterTasks(AppState& s) {
    std::vector<ops::OpStatus> tasks;
    std::vector<AppState::OperationCenterTaskSource> sources;
    std::unordered_map<HWND, std::unordered_map<uint64_t, const AppState::OperationCenterTaskSource*>> previous;
    for (const auto& source : s.operationCenterSources) previous[source.hwnd][source.task_id] = &source;
    const auto append = [&](AppState& window) {
        for (auto task : window.ops.TaskStatuses()) {
            const auto& old_tasks = previous[window.hwnd];
            const auto found = old_tasks.find(task.task_id);
            const auto display_id = found != old_tasks.end() && found->second->window == &window
                ? found->second->display_id : s.nextOperationCenterId++;
            sources.push_back({&window, window.hwnd, task.task_id, display_id});
            task.task_id = display_id;
            tasks.push_back(std::move(task));
        }
    };
    s.core.windows.ForEach([&](HWND, AppState& window) { append(window); });
    // Standalone fixtures and initialization can precede window registration.
    if (s.core.windows.Lookup(s.hwnd) != &s) append(s);
    s.operationCenterSources = std::move(sources);
    return tasks;
}

AppState* ResolveOperationCenterTask(AppState& s, uint64_t& task) {
    const auto found = std::find_if(s.operationCenterSources.begin(), s.operationCenterSources.end(),
        [task](const auto& source) { return source.display_id == task; });
    if (found == s.operationCenterSources.end()) return nullptr;
    if (s.core.windows.Lookup(found->hwnd) != found->window &&
        !(found->window == &s && found->hwnd == s.hwnd)) return nullptr;
    task = found->task_id;
    return found->window;
}

void SyncOperationCenters(AppState& s) {
    s.core.windows.ForEach([&](HWND hwnd, AppState& window) {
        UpdateOperationCenter(window);
        InvalidateRect(hwnd, nullptr, FALSE);
    });
    if (s.core.windows.Lookup(s.hwnd) != &s) UpdateOperationCenter(s);
}

void UpdateOperationCenter(AppState& s) {
    for (auto& outcome : s.ops.DrainTransferOutcomes()) {
        const auto found = s.trayTransfers.find(outcome.task_id);
        if (found == s.trayTransfers.end()) continue;
        found->second.can_restage = outcome.can_restage;
        found->second.remaining = std::move(outcome.remaining_sources);
    }
    const auto tasks = s.ops.TaskStatuses();
    std::erase_if(s.trayTransfers, [&](const auto& record) {
        return std::none_of(tasks.begin(), tasks.end(), [&](const auto& task) { return task.task_id == record.first; });
    });
    if (s.operationCenter) s.operationCenter->Update(CollectOperationCenterTasks(s));
}

void InitializeOperationCenter(AppState& s) {
    if (s.operationCenter || s.exit_requested || s.ui_schedule.stopped || !s.hwnd) return;
    s.operationCenter = std::make_unique<ui::OperationCenter>();
    if (!s.operationCenter->Create(s.hwnd, &s.compositor)) {
        s.operationCenter.reset();
        return;
    }
    const HWND owner = s.hwnd;
    ui::OperationCenterCallbacks callbacks;
    callbacks.can_resume_incomplete = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        const auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr;
        if (!state) return false;
        const auto found = state->trayTransfers.find(task);
        return (found == state->trayTransfers.end() || !found->second.recovery_consumed) &&
            state->ops.CanResumeTransfer(task);
    };
    callbacks.resume_incomplete = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        if (auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr) {
            const auto next = state->ops.ResumeIncompleteTransfer(task);
            if (next) {
                const auto found = state->trayTransfers.find(task);
                if (found != state->trayTransfers.end()) state->trayTransfers[next].move_intent = found->second.move_intent;
                state->trayTransfers.erase(task);
            }
            SyncOperationCenters(*state);
            InvalidateRect(owner, nullptr, FALSE);
        }
    };
    callbacks.can_return_to_tray = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        const auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr;
        if (!state) return false;
        const auto found = state->trayTransfers.find(task);
        return found != state->trayTransfers.end() && found->second.can_restage && !found->second.remaining.empty();
    };
    callbacks.return_to_tray = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr;
        if (!state) return;
        const auto found = state->trayTransfers.find(task);
        if (found == state->trayTransfers.end() || !found->second.can_restage || found->second.remaining.empty()) return;
        state->tray.Collect(found->second.remaining, found->second.move_intent);
        found->second.recovery_consumed = true;
        found->second.can_restage = false;
        found->second.remaining.clear();
        SyncOperationCenters(*state);
        InvalidateRect(owner, nullptr, FALSE);
    };
    callbacks.clear_finished = [owner] {
        if (auto* state = GetAppState(owner)) {
            state->core.windows.ForEach([](HWND, AppState& window) { window.ops.ClearFinishedTasks(); });
            if (state->core.windows.Lookup(state->hwnd) != state) state->ops.ClearFinishedTasks();
            SyncOperationCenters(*state);
            InvalidateRect(owner, nullptr, FALSE);
        }
    };
    callbacks.pause = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        if (auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr) state->ops.PauseTask(task);
    };
    callbacks.resume = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        if (auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr) state->ops.ResumeTask(task);
    };
    callbacks.cancel = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        if (auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr) state->ops.CancelTask(task);
    };
    callbacks.details = [owner](uint64_t task) {
        auto* viewer = GetAppState(owner);
        auto* state = viewer ? ResolveOperationCenterTask(*viewer, task) : nullptr;
        if (!state) return;
        const auto tasks = state->ops.TaskStatuses();
        const auto found = std::find_if(tasks.begin(), tasks.end(),
            [task](const auto& status) { return status.task_id == task; });
        if (found == tasks.end()) return;
        viewer->operationCenter->Hide(false);
        if (found->active) {
            if (state->ops.Status().task_id == task) PinAndShowOperationWindow(*state);
            return;
        }
        if (found->phase == ops::OpPhase::Queued) return;
        if (!viewer->operationHistoryWindow) {
            viewer->operationHistoryWindow = std::make_unique<ui::FileOperationWindow>();
            ui::FileOperationCallbacks history_callbacks;
            history_callbacks.dismiss = [owner] {
                if (auto* current = GetAppState(owner); current && current->operationHistoryWindow)
                    current->operationHistoryWindow->Hide();
            };
            ui::FileOperationWindowOptions options;
            options.use_summary_title = true;
            if (!viewer->operationHistoryWindow->Create(owner, std::move(history_callbacks), options)) {
                viewer->operationHistoryWindow.reset();
                return;
            }
        }
        viewer->operationHistoryWindow->SetTheme(viewer->darkMode, viewer->accentColor);
        viewer->operationHistoryWindow->Update(*found);
        viewer->operationHistoryWindow->Show();
    };
    callbacks.open_destination = [owner](const std::wstring& destination) {
        if (auto* state = GetAppState(owner); state && !destination.empty() &&
            !fs::IsVirtualPath(destination)) {
            state->operationCenter->Hide(false);
            NavigateTo(*state, destination);
        }
    };
    s.operationCenter->SetCallbacks(std::move(callbacks));
    UpdateOperationCenterTheme(s);
    UpdateOperationCenter(s);
}

void ToggleOperationCenter(AppState& s) {
    InitializeOperationCenter(s);
    if (!s.operationCenter) return;
    const float width = static_cast<float>(s.compositor.Width());
    const auto layout = ui::MakeToolbarLayout(width, s.scale, s.renderer.TitleBarHeight(),
        s.renderer.Margin(), 32*s.scale, s.renderer.EffectiveSidebarWidth(width));
    POINT points[]{{static_cast<LONG>(layout.operations.left), static_cast<LONG>(layout.operations.top)},
        {static_cast<LONG>(layout.operations.right), static_cast<LONG>(layout.operations.bottom)}};
    MapWindowPoints(s.hwnd, nullptr, points, 2);
    if (!s.operationCenter->IsVisible()) {
        UpdateOperationCenterTheme(s);
        UpdateOperationCenter(s);
    }
    s.operationCenter->Toggle({points[0].x, points[0].y, points[1].x, points[1].y});
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
