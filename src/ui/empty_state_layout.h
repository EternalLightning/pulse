// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <d2d1.h>
#include <algorithm>
namespace pulse::ui {
struct PaneEmptyLayout {
    D2D1_RECT_F art{};
    D2D1_RECT_F title{};
    D2D1_RECT_F message{};
    D2D1_RECT_F action{};
    bool show_message = false;
    bool show_action = false;
};

inline PaneEmptyLayout MakePaneEmptyLayout(const D2D1_RECT_F& bounds, float scale,
                                    bool can_create, float art_aspect = 512.0f / 360.0f) {
    PaneEmptyLayout out;
    const float width = std::max(0.0f, bounds.right - bounds.left);
    const float height = std::max(0.0f, bounds.bottom - bounds.top);
    out.show_message = height >= 220.0f * scale;
    out.show_action = can_create && height >= 280.0f * scale && width >= 180.0f * scale;

    const float titleH = 24.0f * scale;
    const float messageH = out.show_message ? 20.0f * scale : 0.0f;
    const float actionH = out.show_action ? 34.0f * scale : 0.0f;
    const float textGap = out.show_message ? 2.0f * scale : 0.0f;
    const float actionGap = out.show_action ? 14.0f * scale : 0.0f;
    const float fixedH = titleH + textGap + messageH + actionGap + actionH;
    const float aspect = art_aspect > 0.05f ? art_aspect : (512.0f / 360.0f);
    const float maxArtW = std::max(72.0f * scale,
        std::min(200.0f * scale, width - 32.0f * scale));
    const float maxArtH = std::max(60.0f * scale, height - fixedH - 44.0f * scale);
    const float artW = std::min(maxArtW, maxArtH * aspect);
    const float artH = artW / aspect;
    // The SVG viewBox already has bottom breathing room; keep only a small
    // layout gap so the illustration and copy read as one centered group.
    const float artGap = 2.0f * scale;
    const float totalH = artH + artGap + fixedH;
    float y = bounds.top + std::max(8.0f * scale, (height - totalH) * 0.5f);
    const float cx = (bounds.left + bounds.right) * 0.5f;
    out.art = D2D1::RectF(cx - artW * 0.5f, y, cx + artW * 0.5f, y + artH);
    y = out.art.bottom + artGap;
    out.title = D2D1::RectF(bounds.left + 12.0f * scale, y,
                            bounds.right - 12.0f * scale, y + titleH);
    y = out.title.bottom + textGap;
    out.message = D2D1::RectF(bounds.left + 12.0f * scale, y,
                              bounds.right - 12.0f * scale, y + messageH);
    y = out.message.bottom + actionGap;
    const float actionW = std::min(142.0f * scale, width - 32.0f * scale);
    out.action = D2D1::RectF(cx - actionW * 0.5f, y,
                             cx + actionW * 0.5f, y + actionH);
    return out;
}

}
