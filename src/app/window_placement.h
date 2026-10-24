#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace pulse::app {

inline RECT ConstrainRestoredWindowRect(const RECT& saved, const RECT& work) {
    const int64_t work_width = static_cast<int64_t>(work.right) - work.left;
    const int64_t work_height = static_cast<int64_t>(work.bottom) - work.top;
    const int64_t saved_width = static_cast<int64_t>(saved.right) - saved.left;
    const int64_t saved_height = static_cast<int64_t>(saved.bottom) - saved.top;
    if (work_width <= 0 || work_height <= 0 || saved_width <= 0 || saved_height <= 0) return saved;
    const auto width = std::min(saved_width, work_width);
    const auto height = std::min(saved_height, work_height);
    const auto left = std::clamp<int64_t>(saved.left, work.left, static_cast<int64_t>(work.right) - width);
    const auto top = std::clamp<int64_t>(saved.top, work.top, static_cast<int64_t>(work.bottom) - height);
    return {static_cast<LONG>(left), static_cast<LONG>(top),
        static_cast<LONG>(left + width), static_cast<LONG>(top + height)};
}

inline RECT ConstrainRestoredWindowRect(const RECT& saved) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST), &info)) return saved;
    return ConstrainRestoredWindowRect(saved, info.rcWork);
}

} // namespace pulse::app
