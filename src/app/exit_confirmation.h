#pragma once
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

namespace pulse::app {
inline ui::ConfirmDialogSpec ExitConfirmationSpec() {
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::ExitPendingTitle);
    spec.message = l10n::Get(l10n::StringId::ExitPendingMessage);
    spec.confirm_text = l10n::Get(l10n::StringId::ExitStopTasks);
    spec.cancel_text = l10n::Get(l10n::StringId::ExitKeepRunning);
    spec.cancel_is_default = true;
    spec.danger = true;
    spec.fit_to_work_area = true;
    return spec;
}
}
