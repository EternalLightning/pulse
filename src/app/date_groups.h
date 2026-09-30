#pragma once
#include "../fs/fs_snapshot.h"
#include "../ui/view_layout.h"
#include <vector>

namespace pulse::app {

// Buckets follow local calendar days; the caller caches this for each snapshot.
std::vector<ui::DateGroup> BuildDateGroups(const fs::SnapshotPtr& snapshot,
    const std::vector<int>* visible_indices, const SYSTEMTIME& today);

} // namespace pulse::app
