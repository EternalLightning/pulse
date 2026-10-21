#pragma once
#include <chrono>
#include <cstdint>

namespace pulse::ui {

inline uint64_t AnimationTimeMs() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace pulse::ui
