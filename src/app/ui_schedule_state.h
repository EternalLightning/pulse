// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::app {

struct UiDeadlinePlan {
    uint64_t due = 0;
    void Include(uint64_t value) noexcept {
        if (value && (!due || value < due)) due = value;
    }
    uint32_t Delay(uint64_t now) const noexcept {
        if (!due) return 0;
        const uint64_t remaining = due > now ? due - now : 0;
        return static_cast<uint32_t>(std::clamp<uint64_t>(remaining, 10, 0x7fffffff));
    }
};

struct UiScheduleState {
    static constexpr uint64_t kQuietAfterMs = 5000;
    uint64_t last_activity = 0;
    uint64_t deadline = 0;
    uint64_t tooltip_checked_since = 0;
    uint64_t options_due = 0;
    bool stopped = false;
    uint32_t maintenance_ms = 0;
    bool animation_active = false;

    void NoteActivity(uint64_t now) noexcept { last_activity = now; }
    uint32_t MaintenanceInterval(bool visible, bool exiting, uint64_t now) const noexcept {
        if (exiting) return 200;
        if (!visible) return 1000;
        return now >= last_activity && now - last_activity >= kQuietAfterMs ? 500 : 100;
    }
};

} // namespace pulse::app
