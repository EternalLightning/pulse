// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::ui {

struct BinaryTransition {
    static constexpr float kHoverEnterMs = 110.0f;
    static constexpr float kHoverExitMs = 150.0f;

    float value = 0.0f;
    float from = 0.0f;
    float target = 0.0f;
    uint64_t started = 0;
    bool initialized = false;

    bool Pending() const noexcept { return value != target; }

    void TickHover(bool hot, uint64_t now, bool animate,
                   float enter_ms = kHoverEnterMs, float exit_ms = kHoverExitMs) noexcept {
        if (!initialized) Tick(false, now, false, enter_ms);
        // Sample the current leg with its own duration before redirecting it.
        Tick(hot, now, animate, target > 0.0f ? enter_ms : exit_ms);
    }

    void Tick(bool on, uint64_t now, bool animate, float duration_ms) noexcept {
        const float next = on ? 1.0f : 0.0f;
        if (!initialized || !animate) {
            initialized = true;
            value = from = target = next;
            started = now;
            return;
        }
        if (Pending()) {
            const float t = std::clamp(static_cast<float>(now >= started ? now - started : 0) / duration_ms, 0.0f, 1.0f);
            const float remaining = 1.0f - t;
            value = t >= 1.0f ? target : from + (target - from) * (1.0f - remaining * remaining * remaining);
        }
        if (next != target) {
            from = value;
            target = next;
            started = now;
        }
    }
};

} // namespace pulse::ui
