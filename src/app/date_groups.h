#pragma once
#include "../fs/fs_snapshot.h"
#include "../ui/view_layout.h"
#include "../model/sort_types.h"
#include <vector>

namespace pulse::app {

// Buckets follow local calendar days; the caller caches this for each snapshot.
std::vector<ui::DateGroup> BuildDateGroups(const fs::SnapshotPtr& snapshot,
    const std::vector<int>* visible_indices, const SYSTEMTIME& today);

struct GroupedView {
    std::vector<int> indices;
    std::vector<int> source_to_view;
    std::vector<ui::DateGroup> groups;
};

// Stable regrouping preserves the existing user sort inside each group and
// keeps all selection identities in the immutable source snapshot.
GroupedView BuildGroupedView(const fs::SnapshotPtr& snapshot,
    const std::vector<int>* visible_indices, ui::GroupMode mode,
    model::SortDirection direction, const SYSTEMTIME& today);

} // namespace pulse::app
