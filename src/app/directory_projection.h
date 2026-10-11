#pragma once
#include "entry_sort.h"
#include "../fs/fs_snapshot.h"
#include <algorithm>
#include <functional>

namespace pulse::app {
inline fs::SnapshotPtr ProjectDirectorySnapshot(const fs::SnapshotPtr& source,
    const std::wstring& path, ui::SortColumn column, ui::SortDirection direction,
    FolderSortMode folders, const std::function<bool()>& cancelled = {}) {
    if (!source || (cancelled && cancelled())) return {};
    size_t comparisons = 0;
    struct Cancelled {};
    const auto less = [&](const fs::DirEntry& left, const fs::DirEntry& right) {
        if ((++comparisons & 8191u) == 0 && cancelled && cancelled()) throw Cancelled{};
        return EntryLess(left, right, column, direction, folders, path);
    };
    try {
        if (std::is_sorted(source->begin(), source->end(), less)) return source;
        auto projection = std::make_shared<std::vector<fs::DirEntry>>(*source);
        std::sort(projection->begin(), projection->end(), less);
        if (cancelled && cancelled()) return {};
        return projection;
    } catch (const Cancelled&) { return {}; }
}
} // namespace pulse::app
