// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../model/view_mode.h"
#include <d2d1.h>
#include <algorithm>
#include <utility>

namespace pulse::app {

struct ViewModeScrollAnchor {
    int source_index = -1;
    float position = 0.0f;
};

template <typename ItemRect, typename SourceIndex>
ViewModeScrollAnchor CaptureViewModeScrollAnchor(model::ViewMode mode,
    D2D1_RECT_F viewport, int selected_view, std::pair<int, int> visible_range,
    ItemRect item_rect, SourceIndex source_index) {
    ViewModeScrollAnchor anchor;
    if (viewport.right <= viewport.left || viewport.bottom <= viewport.top) return anchor;
    const auto capture = [&](int view) {
        if (view < 0) return false;
        const auto item = item_rect(view);
        if (item.right <= viewport.left || item.left >= viewport.right ||
            item.bottom <= viewport.top || item.top >= viewport.bottom) return false;
        const int source = source_index(view);
        if (source < 0) return false;
        const bool horizontal = mode == model::ViewMode::List;
        const float start = horizontal ? item.left - viewport.left : item.top - viewport.top;
        const float travel = horizontal
            ? viewport.right - viewport.left - (item.right - item.left)
            : viewport.bottom - viewport.top - (item.bottom - item.top);
        anchor = {source, travel > 0.0f ? std::clamp(start / travel, 0.0f, 1.0f) : 0.0f};
        return true;
    };
    if (capture(selected_view)) return anchor;
    // Renderer visibility ranges include overscan, so test actual intersection.
    for (int view = visible_range.first; view >= 0 && view <= visible_range.second; ++view) {
        if (capture(view)) break;
    }
    return anchor;
}

inline D2D1_POINT_2F RestoreViewModeScrollAnchor(const ViewModeScrollAnchor& anchor,
    model::ViewMode mode, D2D1_RECT_F viewport, D2D1_RECT_F unscrolled_item,
    float max_x, float max_y) {
    if (anchor.source_index < 0) return {};
    const bool horizontal = mode == model::ViewMode::List;
    const float travel = horizontal
        ? viewport.right - viewport.left - (unscrolled_item.right - unscrolled_item.left)
        : viewport.bottom - viewport.top - (unscrolled_item.bottom - unscrolled_item.top);
    const float start = horizontal ? unscrolled_item.left - viewport.left
                                   : unscrolled_item.top - viewport.top;
    const float offset = start - anchor.position * std::max(0.0f, travel);
    return horizontal ? D2D1_POINT_2F{std::clamp(offset, 0.0f, max_x), 0.0f}
                      : D2D1_POINT_2F{0.0f, std::clamp(offset, 0.0f, max_y)};
}

} // namespace pulse::app
