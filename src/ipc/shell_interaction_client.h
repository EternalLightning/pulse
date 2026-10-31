// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "shell_launch_protocol.h"

namespace pulse::ipc {
struct ShellInteractionResult {
    HRESULT error = E_FAIL;
    bool cancelled = false;
    std::wstring value;
};

// Shell runs in a launch helper; the caller's modal loop only dispatches messages.
ShellInteractionResult RunShellInteraction(const ShellLaunchRequest& request);
bool PickShellPath(HWND owner, bool folder, const std::wstring& title, std::wstring& path,
                   const std::vector<std::wstring>& filters = {}, const std::wstring& initial = {});
}
