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
    uint64_t deadline = 0;
    uint64_t tooltip_checked_since = 0;
    uint64_t options_due = 0;
    bool stopped = false;
    uint32_t maintenance_ms = 0;
    bool animation_active = false;
};

} // namespace pulse::app
