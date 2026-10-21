#pragma once
#include "filter_animation.h"
#include "../ui/animation_clock.h"

namespace pulse::app {

inline uint64_t PaneAnimationTimeMs() noexcept {
    return ui::AnimationTimeMs();
}

inline bool PaneAnimationsPending(const Pane& pane, bool focused, bool editing) noexcept {
    const float header_target = focused ? 1.0f : 0.0f;
    const float filter_target = editing || !pane.view.filter_text.empty() ? 1.0f : 0.0f;
    return !pane.header_animation.initialized ||
        pane.header_animation.target != header_target ||
        pane.header_animation.opacity != header_target ||
        pane.filter_animation_target != filter_target || pane.filter_expand != filter_target;
}

inline bool TickPaneAnimations(Pane& pane, bool focused, bool editing, uint64_t now, bool animate = true) {
    pane.header_animation.Tick(focused, now, animate);
    TickFilterAnimation(pane, editing, now, animate);
    // A newly started transition still needs another frame even when its first
    // sample has zero elapsed time and has not changed the visible value yet.
    return PaneAnimationsPending(pane, focused, editing);
}

} // namespace pulse::app
