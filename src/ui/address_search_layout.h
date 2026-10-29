#pragma once
#include <d2d1.h>
#include <algorithm>

namespace pulse::ui {
inline float AddressSearchContentOpacity(float progress) {
    const float t = std::clamp(progress * 2.0f - 1.0f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct AddressSearchLayout {
    D2D1_RECT_F scope, mode, name, content, input, clear, options, close;
    bool scope_label = false;
    bool mode_label = false;
};
inline AddressSearchLayout LayoutAddressSearch(D2D1_RECT_F field, float scale) {
    AddressSearchLayout out;
    const float width = (field.right - field.left) / scale;
    const float top = field.top + 3 * scale, bottom = field.bottom - 3 * scale;
    const float button = (width < 140 ? 14.0f : 24.0f) * scale;
    out.scope_label = width >= 440;
    out.mode_label = width >= 440;
    float x = field.left + 4 * scale;
    const float scope_width = (out.scope_label ? 128.0f : width >= 380 ? 28.0f : 0.0f) * scale;
    out.scope = D2D1::RectF(x, top, x + scope_width, bottom);
    x = out.scope.right + (scope_width > 0 ? 4 * scale : 0);
    const float segment = (out.mode_label ? 80.0f : width < 140 ? 12.0f : 26.0f) * scale;
    out.name = D2D1::RectF(x, top, x + segment, bottom);
    out.content = D2D1::RectF(out.name.right, top, out.name.right + segment, bottom);
    out.mode = D2D1::RectF(x, top, out.content.right, bottom);
    const float right = field.right - 4 * scale;
    out.close = D2D1::RectF(right - (width >= 140 ? button : 0), top, right, bottom);
    out.options = D2D1::RectF(out.close.left - button, top, out.close.left, bottom);
    out.clear = D2D1::RectF(out.options.left - (width >= 480 ? button : 0), top, out.options.left, bottom);
    out.input = D2D1::RectF(out.mode.right + 4 * scale, field.top + 2 * scale,
                           out.clear.left - 4 * scale, field.bottom - 2 * scale);
    return out;
}
} // namespace pulse::ui
