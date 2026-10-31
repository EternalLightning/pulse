// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../fs/fs_enum.h"
#include <set>

namespace pulse::app {

inline std::vector<std::wstring> CloudRefreshParents(const std::vector<std::wstring>& paths) {
    const auto less = [](const std::wstring& left, const std::wstring& right) {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    };
    std::set<std::wstring, decltype(less)> seen(less);
    std::vector<std::wstring> parents;
    for (const auto& path : paths) {
        auto parent = fs::ParentPath(path);
        if (seen.insert(parent).second) parents.push_back(std::move(parent));
    }
    return parents;
}

} // namespace pulse::app
