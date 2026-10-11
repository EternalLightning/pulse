#pragma once

#include "../ui/file_operation_dialog.h"

namespace pulse {
struct AppState;

ui::ConfirmDialogSpec BuildPermissionOperationConfirmationSpec(const ops::OpStatus& status);
void PumpPermissionOperationPrompt(AppState& state);
}
