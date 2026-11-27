// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "../ui/file_operation_dialog.h"

namespace pulse {
void PresentDeleteConfirmation(AppState& s) {
    const auto pending = s.ops.PendingDeleteConfirmation();
    if (!pending || pending->token == s.deleteUiToken) return;
    if (s.operationWindow && s.operationWindow->IsVisible()) s.operationWindow->Hide();
    s.deleteUiToken = pending->token; // Set before the modal loop can reenter WM_OPS_NOTIFY.
    auto spec = ui::BuildDeleteConfirmationSpec(*pending);
    const uint64_t token = pending->token;
    spec.still_valid = [&s, token] {
        const auto current = s.ops.PendingDeleteConfirmation();
        return current && current->token == token && IsWindow(s.hwnd);
    };
    const bool accepted = ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    s.ops.ResolveDeleteConfirmation(token, accepted);
}
}
