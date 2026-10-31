// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::ui::playback {
inline double Fraction(double x, double left, double right) noexcept {
    if (!(right > left) || !std::isfinite(x)) return 0;
    return std::clamp((x - left) / (right - left), 0.0, 1.0);
}
inline uint32_t FrameAt(double fraction, uint32_t count) noexcept {
    if (count < 2 || !std::isfinite(fraction)) return 0;
    return static_cast<uint32_t>(std::llround(std::clamp(fraction, 0.0, 1.0) * (count - 1)));
}
inline uint32_t StepFrame(uint32_t frame, int direction, uint32_t count) noexcept {
    if (!count) return 0;
    frame = (std::min)(frame, count - 1);
    if (direction < 0) return frame ? frame - 1 : 0;
    return frame < count - 1 ? frame + 1 : count - 1;
}
} // namespace pulse::ui::playback
