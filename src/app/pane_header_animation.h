// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::app {
struct PaneHeaderAnimation {
    float opacity = 0.0f;
    float from = 0.0f;
    float target = 0.0f;
    uint64_t started = 0;
    bool initialized = false;

    bool Tick(bool focused, uint64_t now, bool animate = true) noexcept {
        const float before = opacity;
        const float next = focused ? 1.0f : 0.0f;
        if (!initialized || !animate) {
            initialized = true;
            opacity = from = target = next;
            started = now;
        } else if (opacity != target) {
            // Sample the old track at this event's time before redirecting it.
            const float t = std::clamp(static_cast<float>(now >= started ? now - started : 0) / 180.0f, 0.0f, 1.0f);
            const float remaining = 1.0f - t;
            opacity = t >= 1.0f ? target : from + (target - from) *
                (1.0f - remaining * remaining * remaining);
        }
        if (next != target) {
            from = opacity;
            target = next;
            started = now;
        }
        return before != opacity;
    }
};
}
