// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <algorithm>
#include <vector>

namespace pulse::ui {
struct DialogFocusTarget {
    int id;
    HWND edit = nullptr;
};

inline void MoveDialogFocus(HWND host, int& focused,
                            const std::vector<DialogFocusTarget>& targets, bool backwards) {
    if (targets.empty()) return;
    const HWND current = GetFocus();
    auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
        return target.edit ? current == target.edit : current == host && target.id == focused;
    });
    const int count = static_cast<int>(targets.size());
    const int index = found == targets.end() ? (backwards ? 0 : count - 1)
        : static_cast<int>(found - targets.begin());
    const auto& next = targets[static_cast<size_t>((index + (backwards ? count - 1 : 1)) % count)];
    focused = next.id;
    SetFocus(next.edit ? next.edit : host);
    InvalidateRect(host, nullptr, FALSE);
}
} // namespace pulse::ui
