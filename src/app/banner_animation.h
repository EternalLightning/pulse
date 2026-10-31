// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/binary_transition.h"
#include <string>

namespace pulse::app {

struct BannerAnimation {
    ui::BinaryTransition expansion;
    std::wstring path;
    std::wstring title;
    std::wstring message;
    std::wstring network_search_root;
    int kind = 0;

    bool MatchesPath(const std::wstring& current_path) const noexcept {
        return expansion.initialized && path == current_path;
    }

    void Tick(const std::wstring& current_path, const std::wstring& next_title,
              const std::wstring& next_message, const std::wstring& next_network_root,
              int next_kind, uint64_t now, bool animate) {
        // Navigation owns the whole pane transition; its banner starts at the
        // destination state instead of replaying an in-page expansion.
        if (!MatchesPath(current_path)) {
            path = current_path;
            expansion = {};
        }
        const bool visible = !next_message.empty();
        expansion.Tick(visible, now, animate, 180.0f);
        if (visible) {
            title = next_title;
            message = next_message;
            network_search_root = next_network_root;
            kind = next_kind;
        } else if (expansion.value == 0.0f) {
            title.clear();
            message.clear();
            network_search_root.clear();
            kind = 0;
        }
    }
};

} // namespace pulse::app
