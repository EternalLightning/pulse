// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/fs_enum.h"
#include "../ui/sort_types.h"
#include <cstdint>
#include <string_view>

namespace pulse::app {

// How folders are ordered against files in the details list.
enum class FolderSortMode : uint8_t {
    FoldersFirst = 0,  // folders stay on top, reversed order included (File Explorer)
    FollowDirection,   // folders lead ascending order and trail descending order
    Mixed,             // one order for folders and files
};

constexpr FolderSortMode FolderSortModeFromInt(int value) noexcept {
    return value == 1 ? FolderSortMode::FollowDirection
         : value == 2 ? FolderSortMode::Mixed
                      : FolderSortMode::FoldersFirst;
}

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir, std::wstring_view parent = {});
// Capture the preference once per sort; comparators must not read mutable state.
bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders,
               std::wstring_view parent = {});

} // namespace pulse::app
