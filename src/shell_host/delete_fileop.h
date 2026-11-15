// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <sherrors.h>

namespace pulse::shell {
// Transfer callback flags do not encode IFileOperation's recycle mode.
// Only actual recycle failures can request a new permanent-delete confirmation.
inline bool NeedsPermanentDeleteConfirmation(HRESULT error) noexcept {
    return error == COPYENGINE_E_RECYCLE_FORCE_NUKE || error == COPYENGINE_E_RECYCLE_SIZE_TOO_BIG ||
        error == COPYENGINE_E_RECYCLE_PATH_TOO_LONG || error == COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
}
}
