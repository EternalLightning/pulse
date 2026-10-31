// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
bool PulseMenuPreviewDragMove(AppState& state, int x, int y, WPARAM buttons);
bool PulseMenuPreviewDragEnd(AppState& state, int x, int y);
bool PulseMenuPreviewDragCancel(AppState& state);
}
