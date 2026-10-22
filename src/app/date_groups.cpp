#include "date_groups.h"
#include <chrono>
#include <algorithm>
#include <cwctype>
#include <map>

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
            groups.push_back({static_cast<int>(row), 1, bucket, {}});
        else
            ++groups.back().count;
    }
    return groups;
}

GroupedView BuildGroupedView(const fs::SnapshotPtr& snapshot,
    const std::vector<int>* visible_indices, ui::GroupMode mode,
    model::SortDirection direction, const SYSTEMTIME& today) {
    GroupedView out;
    if (!snapshot || mode == ui::GroupMode::None || mode == ui::GroupMode::Auto) return out;
    const Boundaries bounds = MakeBoundaries(today);
    struct Key {
        int bucket = 0;
        std::wstring extension;
        bool operator<(const Key& other) const {
            return bucket != other.bucket ? bucket < other.bucket : extension < other.extension;
        }
    };
    std::map<Key, std::vector<int>> buckets;
    const size_t count = visible_indices ? visible_indices->size() : snapshot->size();
    for (size_t row = 0; row < count; ++row) {
        const int source = visible_indices ? (*visible_indices)[row] : static_cast<int>(row);
        if (source < 0 || static_cast<size_t>(source) >= snapshot->size()) continue;
        const auto& entry = (*snapshot)[static_cast<size_t>(source)];
        Key key;
        if (mode == ui::GroupMode::DateModified) {
            // Ascending group order follows time, from oldest to newest.
            key.bucket = 5 - DateBucket(entry.mtime, bounds);
        } else if (mode == ui::GroupMode::Type) {
            key.bucket = entry.is_dir ? 0 : 1;
            if (!entry.is_dir) {
                const size_t dot = entry.name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot != 0 && dot + 1 < entry.name.size()) {
                    key.extension = entry.name.substr(dot);
                    for (auto& c : key.extension) c = static_cast<wchar_t>(std::towlower(c));
                }
            }
        } else if (mode == ui::GroupMode::Size) {
            key.bucket = entry.is_dir ? 0 : entry.size == 0 ? 1 : entry.size < 16 * 1024ull ? 2 :
                entry.size < 1024 * 1024ull ? 3 : entry.size < 128 * 1024 * 1024ull ? 4 :
                entry.size < 1024 * 1024 * 1024ull ? 5 : 6;
        } else return out;
        buckets[std::move(key)].push_back(source);
    }
    out.indices.reserve(count);
    out.source_to_view.assign(snapshot->size(), -1);
    auto append = [&](const auto& item) {
        const auto& [key, indices] = item;
        ui::DateGroup group;
        group.first = static_cast<int>(out.indices.size());
        group.count = static_cast<int>(indices.size());
        group.mode = mode;
        group.bucket = static_cast<unsigned char>(key.bucket);
        if (mode == ui::GroupMode::DateModified) group.bucket = static_cast<unsigned char>(5 - key.bucket);
        else if (mode == ui::GroupMode::Type) {
            group.label = key.extension;
        } else {
            constexpr const wchar_t* labels[] = {L"", L"0 B", L"1 B – <16 KiB", L"16 KiB – <1 MiB",
                L"1 MiB – <128 MiB", L"128 MiB – <1 GiB", L"≥1 GiB"};
            group.label = labels[key.bucket];
        }
        out.groups.push_back(std::move(group));
        for (int source : indices) {
            out.source_to_view[static_cast<size_t>(source)] = static_cast<int>(out.indices.size());
            out.indices.push_back(source);
        }
    };
    if (direction == model::SortDirection::Desc)
        for (auto it = buckets.rbegin(); it != buckets.rend(); ++it) append(*it);
    else for (const auto& item : buckets) append(item);
    return out;
}

} // namespace pulse::app
