// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "filter_animation.h"
#include "../ui/animation_clock.h"

namespace pulse::app {

inline uint64_t PaneAnimationTimeMs() noexcept {
    return ui::AnimationTimeMs();
}

inline bool TickHomeGroupAnimations(Tab& tab, uint64_t now, bool animate) noexcept {
    bool pending = false;
    for (size_t group = 0; group < tab.home_group_transitions.size(); ++group) {
        auto& transition = tab.home_group_transitions[group];
        transition.Tick(!(tab.home_collapsed_mask & (1u << group)), now, animate, 200.0f);
        pending |= transition.Pending();
    }
    return pending;
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
    bool home_pending = false;
    if (auto* tab = pane.ActiveTab()) {
        tab->banner_animation.Tick(tab->current_path, tab->banner_title, tab->banner_message,
            tab->network_search_root, tab->net_readonly ? 2 : 0, now, animate);
        home_pending |= tab->banner_animation.expansion.Pending();
        if (tab->current_path == L"pulse:home") home_pending |= TickHomeGroupAnimations(*tab, now, animate);
        else tab->home_group_transitions = {};
    }
    return PaneAnimationsPending(pane, focused, editing) || home_pending;
}

} // namespace pulse::app
