#pragma once

#include <algorithm>
#include <array>

namespace pulse::ui {

// Reserve readable name/path space, then give metadata its measured width.
// Long type descriptions yield first on narrow panes, preserving complete
// dates and sizes whenever possible.
template<size_t N>
inline void FitDetailsMetadata(std::array<float, N>& widths, int count,
                               const std::array<float, 3>& measured, float scale,
                               float cloud_width = 0.0f) {
    const int first = count - 3;
    const int flexible_count = first - (cloud_width > 0.0f ? 1 : 0);
    float total = 0.0f;
    for (int i = 0; i < count; ++i) total += widths[i];
    total -= cloud_width;
    const float flexible_minimum = (flexible_count == 2 ? 190.0f : 80.0f) * scale;
    const float reserved = std::min(flexible_minimum, total * 0.45f);
    const float desired = measured[0] + measured[1] + measured[2];
    const float available = std::max(0.0f, total - reserved);
    auto fitted = measured;
    if (desired > available) {
        const float compact_type = std::min(measured[1], 64.0f * scale);
        const float fixed = measured[0] + measured[2];
        if (available >= fixed + compact_type) {
            fitted[1] = available - fixed;
        } else {
            const float factor = desired > 0.0f ? available / desired : 0.0f;
            for (float& width : fitted) width *= factor;
        }
    }
    const float remaining = total - fitted[0] - fitted[1] - fitted[2];
    const float name_share = flexible_count == 2 && widths[0] + widths[1] > 0.0f
        ? widths[0] / (widths[0] + widths[1]) : 1.0f;
    widths[0] = remaining * name_share;
    if (flexible_count == 2) {
        widths[0] = remaining >= flexible_minimum
            ? std::clamp(widths[0], 80.0f * scale, remaining - 110.0f * scale)
            : remaining * (80.0f / 190.0f);
        widths[1] = remaining - widths[0];
    }
    for (int i = 0; i < 3; ++i) widths[first + i] = fitted[i];
}

} // namespace pulse::ui
