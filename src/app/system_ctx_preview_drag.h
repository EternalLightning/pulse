// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>

namespace pulse {
struct AppState;
bool SysCtxPreviewDragMove(AppState& state, int x, int y, WPARAM buttons);
bool SysCtxPreviewDragEnd(AppState& state, int x, int y);
bool SysCtxPreviewDragCancel(AppState& state);
}
