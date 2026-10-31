// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>

namespace pulse::app {

// Match the drawable wallpaper area, preserving window area where constraints allow.
inline SIZE FitBackgroundWindowSize(SIZE image, SIZE client, SIZE frame,
                                    SIZE minimum, SIZE maximum) noexcept {
    if (image.cx <= 0 || image.cy <= 0 || maximum.cx <= frame.cx ||
        maximum.cy <= frame.cy) return {};
    const double max_width = maximum.cx - frame.cx;
    const double max_height = maximum.cy - frame.cy;
    const double min_width = std::clamp<double>(minimum.cx - frame.cx, 1, max_width);
    const double min_height = std::clamp<double>(minimum.cy - frame.cy, 1, max_height);
    const double ratio = std::clamp(static_cast<double>(image.cx) / image.cy,
        min_width / max_height, max_width / min_height);
    const double lower = std::max(min_height, min_width / ratio);
    const double upper = std::max(lower, std::min(max_height, max_width / ratio));
    const double area = static_cast<double>(std::max(1L, client.cx)) * std::max(1L, client.cy);
    const double height = std::clamp(std::sqrt(area / ratio), lower, upper);
    return {static_cast<LONG>(std::lround(height * ratio)) + frame.cx,
            static_cast<LONG>(std::lround(height)) + frame.cy};
}

} // namespace pulse::app
