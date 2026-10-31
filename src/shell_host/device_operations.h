// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse::shell {
struct DeviceOperation {
    HWND owner = nullptr;
    std::wstring verb;
    std::wstring name;
    std::vector<std::wstring> ids;
};
// Called on the host STA; clipboard objects stay on that apartment.
HRESULT ExecuteDeviceOperation(const DeviceOperation& request);
}
