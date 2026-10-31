// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::ui {

// Overlapping edge motion stretches the line before the trailing edge catches up.
// Retarget from the sampled span so rapid reversals never jump to an old tab.
struct TabIndicatorAnimation {
    float left = 0.0f;
    float right = 0.0f;
    const void* identity = nullptr;
    bool initialized = false;
    bool pending = false;

    void Reset() noexcept { *this = {}; }

    void Tick(const void* next_identity, float next_left, float next_right,
              uint64_t now, bool animate) noexcept {
        next_right = std::max(next_left, next_right);
        if (!initialized || !animate) {
            Snap(next_identity, next_left, next_right);
            return;
        }
        Sample(now);
        if (identity == next_identity) {
            // Layout changes and drag motion should follow the tab directly.
            if (target_left != next_left || target_right != next_right)
                Snap(next_identity, next_left, next_right);
            return;
        }
        identity = next_identity;
        from_left = left;
        from_right = right;
        target_left = next_left;
        target_right = next_right;
        stretch = next_left < left || next_right > right;
        toward_right = next_left + next_right >= left + right;
        started = now;
        pending = left != target_left || right != target_right;
    }

private:
    float from_left = 0.0f, from_right = 0.0f;
    float target_left = 0.0f, target_right = 0.0f;
    uint64_t started = 0;
    bool stretch = false;
    bool toward_right = false;

    void Snap(const void* next_identity, float next_left, float next_right) noexcept {
        identity = next_identity;
        left = target_left = next_left;
        right = target_right = next_right;
        initialized = true;
        pending = false;
    }

    // Zero velocity and acceleration at both ends, including the delayed edge.
    static float Ease(float t) noexcept { return t * t * t * (t * (6.0f * t - 15.0f) + 10.0f); }

    void Sample(uint64_t now) noexcept {
        if (!pending) return;
        const uint64_t elapsed = now >= started ? now - started : 0;
        const float duration = stretch ? 240.0f : 140.0f;
        const float t = std::clamp(static_cast<float>(elapsed) / duration, 0.0f, 1.0f);
        if (t >= 1.0f) {
            left = target_left;
            right = target_right;
            pending = false;
        } else {
            const float leading = Ease(stretch ? std::clamp(t / 0.74f, 0.0f, 1.0f) : t);
            const float trailing = Ease(stretch ? std::clamp((t - 0.16f) / 0.84f, 0.0f, 1.0f) : t);
            left = from_left + (target_left - from_left) * (toward_right ? trailing : leading);
            right = from_right + (target_right - from_right) * (toward_right ? leading : trailing);
        }
    }
};
} // namespace pulse::ui
