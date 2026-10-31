// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace pulse::ui {

struct EditCaretBlink {
    uint64_t started = 0;
    bool initialized = false;
    bool moving = false;

    void Reset(uint64_t now) {
        started = now;
        initialized = true;
    }
    void Sample(uint64_t now, bool motion_pending) {
        // Start a full visible period when the last motion frame settles.
        if (!initialized || motion_pending || moving) started = now;
        initialized = true;
        moving = motion_pending;
    }
    bool Visible(uint64_t now, uint32_t interval) const {
        if (interval == UINT32_MAX || moving) return true;
        const uint64_t elapsed = now >= started ? now - started : 0;
        return (elapsed / (interval ? interval : 500)) % 2 == 0;
    }
    uint32_t NextDelay(uint64_t now, uint32_t interval) const {
        if (interval == UINT32_MAX) return 0;
        if (!interval) interval = 500;
        const uint64_t elapsed = now >= started ? now - started : 0;
        return interval - static_cast<uint32_t>(elapsed % interval);
    }
};

inline constexpr float kEditMotionDuration = 80;

inline float EditMotionProgress(uint64_t now, uint64_t started, float duration) {
    const float t = std::clamp(static_cast<float>(now >= started ? now - started : 0) / duration, 0.0f, 1.0f);
    const float remaining = 1 - t;
    return t >= 1 ? 1 : 1 - remaining * remaining * remaining;
}

struct EditCaretMotion {
    std::array<float, 3> value{}, from{}, target{};
    uint64_t started = 0;
    bool initialized = false;
    bool pending = false;

    void Tick(std::array<float, 3> next, uint64_t now, bool animate) {
        if (!initialized || !animate) {
            initialized = true; pending = false;
            value = from = target = next;
            return;
        }
        if (pending) {
            const float p = EditMotionProgress(now, started, kEditMotionDuration);
            for (size_t i = 0; i < value.size(); ++i) value[i] = from[i] + (target[i] - from[i]) * p;
            if (p == 1) { value = target; pending = false; }
        }
        if (next != target) {
            from = value; target = next; started = now;
            pending = value != target;
        }
    }
};

struct EditTextMotion {
    size_t prefix = 0, old_end = 0, new_end = 0;
    uint64_t started = 0;
    float progress = 1;
    bool pending = false;

    void Sample(uint64_t now) {
        if (!pending) return;
        progress = EditMotionProgress(now, started, kEditMotionDuration);
        pending = progress < 1;
    }
    void Begin(std::wstring_view before, std::wstring_view after, uint64_t now) {
        prefix = 0;
        while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) ++prefix;
        size_t suffix = 0;
        while (suffix < before.size() - prefix && suffix < after.size() - prefix &&
               before[before.size() - suffix - 1] == after[after.size() - suffix - 1]) ++suffix;
        // Never place a clipping boundary between a UTF-16 surrogate pair.
        if (prefix && prefix < after.size() && after[prefix] >= 0xDC00 && after[prefix] <= 0xDFFF) --prefix;
        if (suffix && suffix < after.size() && after[after.size() - suffix] >= 0xDC00 &&
            after[after.size() - suffix] <= 0xDFFF) --suffix;
        old_end = before.size() - suffix;
        new_end = after.size() - suffix;
        started = now; progress = 0;
        pending = before != after;
    }
};

inline constexpr uintptr_t kEditMotionTimer = 0x504D;
inline constexpr uintptr_t kEditCaretTimer = 71;
} // namespace pulse::ui
