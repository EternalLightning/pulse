// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }
bool TickAppSurfaceScroll(AppState& s, ui::WindowViewModel& vm, uint64_t now);
void CancelAppSurfaceScroll(AppState& s);
void StartHorizontalSmoothScroll(AppState& s, float delta, float maximum, float viewport, uint64_t now);
}
