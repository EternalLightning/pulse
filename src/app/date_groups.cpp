#include "date_groups.h"
#include <chrono>

namespace pulse::app {
namespace {

using namespace std::chrono;

sys_days CalendarDay(const SYSTEMTIME& value) {
    return sys_days{year{value.wYear} / month{value.wMonth} / day{value.wDay}};
}

uint64_t UtcMidnight(sys_days day_value) {
    const year_month_day date{day_value};
    SYSTEMTIME local_date{};
    local_date.wYear = static_cast<WORD>(static_cast<int>(date.year()));
    local_date.wMonth = static_cast<WORD>(static_cast<unsigned>(date.month()));
    local_date.wDay = static_cast<WORD>(static_cast<unsigned>(date.day()));
    SYSTEMTIME utc_date{};
    FILETIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local_date, &utc_date) ||
        !SystemTimeToFileTime(&utc_date, &utc)) return 0;
    return (static_cast<uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
}

struct Boundaries {
    uint64_t today = 0, yesterday = 0, week = 0, last_week = 0, month = 0;
};

Boundaries MakeBoundaries(const SYSTEMTIME& today) {
    const sys_days now = CalendarDay(today);
    const sys_days week = now - days{(weekday{now}.iso_encoding() + 6) % 7};
    const sys_days month_start = sys_days{year{today.wYear} / month{today.wMonth} / day{1}};
    return {UtcMidnight(now), UtcMidnight(now - days{1}), UtcMidnight(week),
        UtcMidnight(week - days{7}), UtcMidnight(month_start)};
}

unsigned char DateBucket(FILETIME utc, const Boundaries& bounds) {
    const uint64_t value = (static_cast<uint64_t>(utc.dwHighDateTime) << 32) |
        utc.dwLowDateTime;
    if (value >= bounds.today) return 0;
    if (value >= bounds.yesterday) return 1;
    if (value >= bounds.week) return 2;
    if (value >= bounds.last_week) return 3;
    if (value >= bounds.month) return 4;
    return 5;
}

} // namespace

std::vector<ui::DateGroup> BuildDateGroups(const fs::SnapshotPtr& snapshot,
    const std::vector<int>* visible_indices, const SYSTEMTIME& today) {
    std::vector<ui::DateGroup> groups;
    if (!snapshot) return groups;
    const Boundaries bounds = MakeBoundaries(today);
    const size_t count = visible_indices ? visible_indices->size() : snapshot->size();
    for (size_t row = 0; row < count; ++row) {
        const int source = visible_indices ? (*visible_indices)[row] : static_cast<int>(row);
        if (source < 0 || static_cast<size_t>(source) >= snapshot->size()) continue;
        const unsigned char bucket = DateBucket((*snapshot)[source].mtime, bounds);
        if (groups.empty() || groups.back().bucket != bucket)
            groups.push_back({static_cast<int>(row), 1, bucket});
        else
            ++groups.back().count;
    }
    return groups;
}

} // namespace pulse::app
