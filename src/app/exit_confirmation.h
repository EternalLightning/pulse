// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

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

inline ui::ConfirmDialogSpec BackgroundCloseConfirmationSpec() {
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::ExitPendingTitle);
    spec.message = l10n::Get(l10n::StringId::ClosePendingBackgroundMessage);
    spec.confirm_text = l10n::Get(l10n::StringId::CloseContinueBackground);
    spec.cancel_text = l10n::Get(l10n::StringId::CloseKeepWindow);
    spec.cancel_is_default = true;
    spec.fit_to_work_area = true;
    return spec;
}
}
