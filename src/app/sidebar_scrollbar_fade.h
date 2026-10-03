#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::app {

class SidebarScrollbarFade {
public:
    bool Tick(uint64_t now, float scroll, bool hovered, bool dragging) {
        if (scroll != last_scroll_) {
            last_scroll_ = scroll;
            visible_until_ = now + 900;
        }
        const float target_opacity = hovered || dragging || now < visible_until_ ? 1.0f : 0.0f;
        const float target_expand = hovered || dragging ? 1.0f : 0.0f;
        const float elapsed = last_tick_ && now >= last_tick_
            ? static_cast<float>(std::min<uint64_t>(now - last_tick_, 200)) : 16.0f;
        last_tick_ = now;
        const float before_opacity = opacity;
        const float before_expand = expand;
        auto approach = [&](float& value, float target, float duration) {
            const float amount = elapsed / duration;
            value += std::clamp(target - value, -amount, amount);
        };
        approach(opacity, target_opacity, target_opacity > opacity ? 70.0f : 200.0f);
        approach(expand, target_expand, 100.0f);
        return opacity != before_opacity || expand != before_expand;
    }
    float opacity = 0.0f;
    float expand = 0.0f;
private:
    uint64_t visible_until_ = 0;
    uint64_t last_tick_ = 0;
    float last_scroll_ = 0.0f;
};

} // namespace pulse::app
