// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <algorithm>

namespace pulse::ui {

inline float FitPopupScale(float requested, float card_width, float card_height,
                           int shadow, const RECT& work) noexcept {
    const float available_width = static_cast<float>((std::max)(0L, work.right - work.left - shadow * 2));
    const float available_height = static_cast<float>((std::max)(0L, work.bottom - work.top - shadow * 2));
    return (std::max)(0.0f, (std::min)(requested,
        (std::min)(available_width / card_width, available_height / card_height)));
}

inline POINT ConstrainPopupOrigin(POINT anchor, SIZE size, const RECT& work) noexcept {
    // An oversized popup stays anchored at the work area's leading edge.
    return {
        (std::max)(work.left, (std::min)(anchor.x, work.right - size.cx)),
        (std::max)(work.top, (std::min)(anchor.y, work.bottom - size.cy))
    };
}

} // namespace pulse::ui
