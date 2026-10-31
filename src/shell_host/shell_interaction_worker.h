// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ipc/shell_launch_protocol.h"
#include <functional>

namespace pulse::shell {
HRESULT PickShellPath(const ipc::ShellLaunchRequest& request, HWND owner, std::wstring& selected);
HRESULT RunElevated(const ipc::ShellLaunchRequest& request, std::wstring& exit_code,
                    const std::function<bool()>& heartbeat);
}
