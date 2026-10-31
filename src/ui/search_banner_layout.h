// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>

namespace pulse::ui {
inline float SearchBannerActionWidth(float width, float scale, bool network_search) {
    return std::min((network_search ? 176.0f : 144.0f) * scale,
                    std::max(0.0f, width) * (network_search ? 0.6f : 0.4f));
}
}
