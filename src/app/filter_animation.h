#pragma once
#include "app_model.h"
#include <algorithm>

namespace pulse::app {
inline bool TickFilterAnimation(Pane& pane, bool editing, uint64_t now, bool animate = true) {
    const float target = editing || !pane.view.filter_text.empty() ? 1.0f : 0.0f;
    const float before = pane.filter_expand;
    if (!animate) {
        pane.filter_expand = pane.filter_animation_from = pane.filter_animation_target = target;
        pane.filter_animation_start = now;
        return before != pane.filter_expand;
    }
    // Retarget from this instant, even when no frame sampled the previous track.
    const float t = std::clamp(static_cast<float>(now >= pane.filter_animation_start ?
        now - pane.filter_animation_start : 0) / 140.0f, 0.0f, 1.0f);
    const float remaining = 1.0f - t;
    pane.filter_expand = t >= 1.0f ? pane.filter_animation_target :
        pane.filter_animation_from + (pane.filter_animation_target - pane.filter_animation_from) *
        (1.0f - remaining * remaining * remaining);
    if (pane.filter_animation_target != target) {
        pane.filter_animation_from = pane.filter_expand;
        pane.filter_animation_target = target;
        pane.filter_animation_start = now;
    }
    return before != pane.filter_expand;
}
}
