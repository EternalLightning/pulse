#include "permission_operation_prompt.h"
#include "app_state.h"
#include "../common/display_path.h"
#include "../common/localization.h"

namespace pulse {
ui::ConfirmDialogSpec BuildPermissionOperationConfirmationSpec(const ops::OpStatus& status) {
    using I = l10n::StringId;
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(I::PermissionOperationTitle);
    spec.message = l10n::Get(I::PermissionOperationMessage);
    spec.confirm_text = l10n::Get(I::PermissionOperationContinue);
    spec.cancel_text = l10n::Get(I::Cancel);
    spec.cancel_is_default = true;
    spec.fit_to_work_area = true;
    spec.note = l10n::Get(I::PermissionOperationUacHint);
    const auto add_path = [&spec](I label, const std::wstring& value) {
        if (value.empty()) return;
        spec.rows.push_back({l10n::Get(label), false, false});
        spec.rows.push_back({path::FriendlyPathText(value), true, true});
    };
    add_path(I::PermissionOperationSourcePath, status.elevation_source_path);
    add_path(I::PermissionOperationDestinationPath, status.elevation_destination_path);
    return spec;
}

void PumpPermissionOperationPrompt(AppState& state) {
    if (state.exit_requested || state.permissionOperationPromptActive ||
        state.lockedOperationPromptActive || !IsWindow(state.hwnd)) return;
    const auto status = state.ops.Status();
    if (status.active || status.phase != ops::OpPhase::Failed ||
        !status.elevation_retry_available || status.lock_retry_available || !status.lock_owners.empty() ||
        !status.task_id || status.task_id == state.permissionOperationPromptedTaskId ||
        !state.ops.IsElevationFailureCurrent(status.task_id)) return;

    // Claim the failed attempt before the modal message loop can reenter.
    state.permissionOperationPromptedTaskId = status.task_id;
    state.operationDismissedTaskId = status.task_id;
    state.operationPinnedByUser = false;
    if (state.operationWindow) state.operationWindow->Hide();
    if (status.type == ops::OpType::CreateFolder || status.type == ops::OpType::CreateTextFile)
        state.pendingRenameName.clear();
    auto spec = BuildPermissionOperationConfirmationSpec(status);
    spec.still_valid = [&state, id = status.task_id] {
        return !state.exit_requested && IsWindow(state.hwnd) &&
            state.ops.IsElevationFailureCurrent(id);
    };
    state.permissionOperationPromptActive = true;
    const bool accepted = ui::ShowConfirmDialog(state.hwnd, spec, state.darkMode, state.accentColor);
    if (accepted && spec.still_valid()) {
        if (!state.ops.RetryElevatedOperation(status.task_id))
            state.ops.CancelElevationRetry(status.task_id);
    } else state.ops.CancelElevationRetry(status.task_id);
    state.permissionOperationPromptActive = false;
    if (IsWindow(state.hwnd) && !state.exit_requested)
        PostMessageW(state.hwnd, WM_OPS_NOTIFY, 0, 0);
}
}
