// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>

namespace pulse::ui {

inline float BannerRevealOpacity(float expansion, float visible_height_dip) noexcept {
    if (expansion < 0.0f) return 1.0f;
    const float reveal = std::clamp((visible_height_dip - 22.0f) / 10.0f, 0.0f, 1.0f);
    return std::clamp(expansion, 0.0f, 1.0f) * reveal * reveal * (3.0f - 2.0f * reveal);
}

} // namespace pulse::ui
