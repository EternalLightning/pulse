// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/fs_snapshot.h"
#include "view_layout.h"
#include "../model/sort_types.h"
#include <vector>

namespace pulse::ui {

struct FileGrouping {
    GroupMode mode = GroupMode::None;
    model::SortDirection direction = model::SortDirection::Asc;
    bool operator==(const FileGrouping&) const = default;
};

constexpr FileGrouping ResolveFileGrouping(ViewMode view, GroupMode group, model::SortColumn sort,
    model::SortDirection sort_direction, model::SortDirection group_direction) noexcept {
    if (view != ViewMode::Details || group == GroupMode::None) return {};
    if (group == GroupMode::Auto)
        return sort == model::SortColumn::Mtime ? FileGrouping{GroupMode::DateModified, sort_direction} : FileGrouping{};
    return {group, group_direction};
}

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

} // namespace pulse::ui
