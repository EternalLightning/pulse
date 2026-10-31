// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::ui {
// The owner keeps its offset and clock. External positioning or a shrinking
// viewport range cancels momentum instead of moving the new content later.
class SmoothScroll {
public:
    void Reset() { pending_ = false; velocity_ = 0.0f; }
    bool Pending() const { return pending_; }

    bool Tick(float& value, float maximum, uint64_t now, bool animate) {
        maximum = std::max(0.0f, maximum);
        const float before = value;
        value = std::clamp(value, 0.0f, maximum);
        if (!pending_) return value != before;
        if (std::abs(value - last_value_) > 0.01f || maximum < maximum_ - 0.01f) {
            Reset();
            return value != before;
        }
        const float elapsed = static_cast<float>(std::min<uint64_t>(now >= last_tick_ ? now - last_tick_ : 0, 100));
        const float decay = animate ? std::exp(-elapsed / kResponseMs) : 0.0f;
        const float next = value + velocity_ * kResponseMs * (1.0f - decay);
        value = std::clamp(next, 0.0f, maximum);
        velocity_ *= decay;
        if (value != next || std::abs(velocity_ * kResponseMs) <= 0.5f) {
            value = std::clamp(value + velocity_ * kResponseMs, 0.0f, maximum);
            Reset();
        }
        last_value_ = value;
        maximum_ = maximum;
        last_tick_ = now;
        return value != before;
    }

    void ScrollBy(float& value, float delta, float maximum, float viewport, uint64_t now, bool animate) {
        Tick(value, maximum, now, animate);
        maximum = std::max(0.0f, maximum);
        if (!animate) {
            value = std::clamp(value + delta, 0.0f, maximum);
            Reset();
            return;
        }
        if (velocity_ * delta < 0.0f) velocity_ = 0.0f;
        const float limit = std::max(1.0f, viewport * 1.5f) / kResponseMs;
        velocity_ = std::clamp(velocity_ + delta / kResponseMs, -limit, limit);
        pending_ = std::abs(std::clamp(value + velocity_ * kResponseMs, 0.0f, maximum) - value) > 0.01f;
        if (!pending_) velocity_ = 0.0f;
        last_value_ = value;
        maximum_ = maximum;
        last_tick_ = now;
    }
private:
    static constexpr float kResponseMs = 105.0f;
    bool pending_ = false;
    float velocity_ = 0.0f;
    float last_value_ = 0.0f;
    float maximum_ = 0.0f;
    uint64_t last_tick_ = 0;
};
} // namespace pulse::ui
