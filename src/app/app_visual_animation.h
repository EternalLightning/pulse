// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cmath>
#include <cstdint>

namespace pulse {
struct AppState;

bool TickVisualAnimations(AppState& s);

namespace app {
inline float AdvanceScrollbarHover(float value, float target, double elapsed_ms) {
    value = target + (value - target) * static_cast<float>(std::pow(0.75, elapsed_ms / 16.0));
    return std::abs(target - value) <= 0.4f ? target : value;
}
} // namespace app
} // namespace pulse
