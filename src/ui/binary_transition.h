#pragma once
#include <algorithm>
#include <cstdint>

namespace pulse::ui {

struct BinaryTransition {
    float value = 0.0f;
    float from = 0.0f;
    float target = 0.0f;
    uint64_t started = 0;
    bool initialized = false;

    bool Pending() const noexcept { return value != target; }

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
