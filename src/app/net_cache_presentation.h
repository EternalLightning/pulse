#pragma once

#include "../common/localization.h"

#include <chrono>
#include <cstdint>

namespace pulse::app {

inline std::wstring FormatCacheAge(uint64_t unix_sec, uint64_t now) {
    using I = l10n::StringId;
    if (!unix_sec || now <= unix_sec) return l10n::Get(I::CacheJustNow);
    const uint64_t seconds = now - unix_sec;
    const auto format = [&](I id, uint64_t count) {
        return l10n::Format(id, static_cast<unsigned long long>(count));
    };
    if (seconds < 60) return format(I::CacheSecondsAgoFormat, seconds);
    if (seconds < 3600) return format(I::CacheMinutesAgoFormat, seconds / 60);
    if (seconds < 86400) return format(I::CacheHoursAgoFormat, seconds / 3600);
    return format(I::CacheDaysAgoFormat, seconds / 86400);
}

inline std::wstring FormatCacheAge(uint64_t unix_sec) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return FormatCacheAge(unix_sec, static_cast<uint64_t>(now));
}

} // namespace pulse::app
