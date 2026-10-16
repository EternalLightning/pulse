#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse {

inline float NormalizeProgressPercent(float percent) noexcept {
    return std::isfinite(percent) && percent >= 0.0f
        ? (std::min)(percent, 100.0f) : -1.0f;
}

inline float WorkProgressPercent(uint64_t done, uint64_t total) noexcept {
    return total ? static_cast<float>(static_cast<double>((std::min)(done, total)) * 100.0 / total)
        : -1.0f;
}

} // namespace pulse
