#include "locked_operation_prompt.h"
#include "app_state.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

namespace pulse {
void PumpLockedOperationPrompt(AppState& state) {
    if (state.exit_requested || state.lockedOperationPromptActive || !IsWindow(state.hwnd)) return;
    const auto status = state.ops.Status();
    const bool rename = status.type == ops::OpType::Rename && status.lock_retry_available;
    if (status.active || status.phase != ops::OpPhase::Failed || (!rename && status.lock_owners.empty()) ||
        !status.task_id || status.task_id == state.lockedOperationPromptedTaskId ||
        !state.ops.IsLockedFailureCurrent(status.task_id)) return;
    state.lockedOperationPromptedTaskId = status.task_id;
    state.operationDismissedTaskId = status.task_id;
    state.operationPinnedByUser = false;
    if (state.operationWindow) state.operationWindow->Hide();
    if (rename) {
        state.lockedOperationPromptActive = true;
        const bool retry = ui::ShowRenameLockedDialog(state.hwnd, status, state.darkMode, state.accentColor,
            [&state, id = status.task_id] {
                return !state.exit_requested && IsWindow(state.hwnd) && state.ops.IsLockedFailureCurrent(id);
            });
        state.lockedOperationPromptActive = false;
        if (retry) state.ops.RetryLockedOperation(status.task_id, false);
        if (IsWindow(state.hwnd) && !state.exit_requested) PostMessageW(state.hwnd, WM_OPS_NOTIFY, 0, 0);
        return;
    }
    using I = l10n::StringId;
    ui::LockedItemDialogText text;
    text.title = l10n::Get(I::LockedItemTitle);
    text.message = l10n::Get(I::LockedItemMessage);
    text.close_hint = l10n::Get(I::LockedItemCloseHint);
    text.end_hint = l10n::Get(I::LockedItemEndHint);
    text.retry = l10n::Get(I::LockedItemRetry);
    text.end_retry = l10n::Get(I::LockedItemEndRetry);
    text.cancel = l10n::Get(I::Cancel);
    state.lockedOperationPromptActive = true;
    const auto choice = ui::ShowLockedItemDialog(state.hwnd, status, text,
        state.darkMode, state.accentColor, [&state, id = status.task_id] {
            return !state.exit_requested && IsWindow(state.hwnd) && state.ops.IsLockedFailureCurrent(id);
        });
    state.lockedOperationPromptActive = false;
    // Cancel/Esc/close dismisses this failed attempt only. It never queues a
    // retry or ends a process; the original failure remains available in history.
    if (choice != ui::LockedItemChoice::Cancel)
        state.ops.RetryLockedOperation(status.task_id, choice == ui::LockedItemChoice::EndAndRetry);
    if (IsWindow(state.hwnd) && !state.exit_requested)
        PostMessageW(state.hwnd, WM_OPS_NOTIFY, 0, 0);
}
}
