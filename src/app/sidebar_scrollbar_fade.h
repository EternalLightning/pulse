// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::app {

class SidebarScrollbarFade {
public:
    bool Tick(double now, float scroll, bool hovered, bool dragging, bool animate = true) {
        if (scroll != last_scroll_) {
            last_scroll_ = scroll;
            visible_until_ = now + 900;
        }
        const float target_opacity = hovered || dragging || now < visible_until_ ? 1.0f : 0.0f;
        const float target_expand = hovered || dragging ? 1.0f : 0.0f;
        const float elapsed = last_tick_ && now >= last_tick_
            ? static_cast<float>(std::min(now - last_tick_, 200.0)) : 0.0f;
        last_tick_ = now;
        const float before_opacity = opacity;
        const float before_expand = expand;
        auto approach = [&](float& value, float target, float previous_target, float duration) {
            if (!animate) {
                value = target;
                return;
            }
            // A target change after an idle deadline must still animate from its current value.
            const float amount = target == previous_target ? elapsed / duration : 0.0f;
            value += std::clamp(target - value, -amount, amount);
        };
        approach(opacity, target_opacity, target_opacity_, target_opacity > opacity ? 70.0f : 200.0f);
        approach(expand, target_expand, target_expand_, 100.0f);
        target_opacity_ = target_opacity;
        target_expand_ = target_expand;
        next_wake_ = !hovered && !dragging && now < visible_until_ ? visible_until_ : 0.0;
        return opacity != before_opacity || expand != before_expand;
    }
    bool Pending() const { return opacity != target_opacity_ || expand != target_expand_; }
    double NextWake() const { return next_wake_; }
    float opacity = 0.0f;
    float expand = 0.0f;
private:
    double visible_until_ = 0;
    double last_tick_ = 0;
    double next_wake_ = 0;
    float target_opacity_ = 0.0f;
    float target_expand_ = 0.0f;
    float last_scroll_ = 0.0f;
};

} // namespace pulse::app
