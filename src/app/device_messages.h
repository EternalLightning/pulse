// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
namespace pulse::app {
inline constexpr UINT WM_DEVICES_CHANGED = WM_APP + 108;
inline constexpr UINT WM_DEVICE_OPERATION_DONE = WM_APP + 109;
}
