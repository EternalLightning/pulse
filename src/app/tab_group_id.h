// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "app_model.h"

#include <algorithm>
#include <limits>

namespace pulse::app {

inline int NextTabGroupId(int id) {
    return id > 0 && id < (std::numeric_limits<int>::max)() ? id + 1 : 1;
}

inline int AllocateTabGroupId(WindowTabs& tabs) {
    int candidate = std::max(1, tabs.next_tab_group_id);
    const int first = candidate;
    do {
        const bool used = std::any_of(tabs.tab_groups.begin(), tabs.tab_groups.end(),
            [candidate](const TabGroup& group) { return group.id == candidate; });
        if (!used) {
            tabs.next_tab_group_id = NextTabGroupId(candidate);
            return candidate;
        }
        candidate = NextTabGroupId(candidate);
    } while (candidate != first);
    return 0;
}

} // namespace pulse::app
