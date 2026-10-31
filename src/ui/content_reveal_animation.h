// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::ui {

// Reveal newly available content once; redraws and zooming do not restart it.
struct ContentRevealAnimation {
    float opacity = 0.0f;
    bool pending = false;

    void Reset() noexcept { *this = {}; }

    void Tick(bool ready, uint64_t now, bool animate) noexcept {
        if (!ready) { Reset(); return; }
        if (!ready_) {
            ready_ = true;
            started_ = now;
            pending = animate;
        }
        Sample(now, animate);
    }

    void Sample(uint64_t now, bool animate) noexcept {
        if (!ready_) return;
        if (!animate) { opacity = 1.0f; pending = false; return; }
        if (!pending) return;
        const auto elapsed = now >= started_ ? now - started_ : 0;
        const float t = std::clamp(static_cast<float>(elapsed) / 160.0f, 0.0f, 1.0f);
        const float remaining = 1.0f - t;
        opacity = t >= 1.0f ? 1.0f : 1.0f - remaining * remaining * remaining;
        pending = t < 1.0f;
    }

private:
    uint64_t started_ = 0;
    bool ready_ = false;
};

} // namespace pulse::ui
