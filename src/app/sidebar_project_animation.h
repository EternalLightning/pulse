// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/ui_renderer.h"

namespace pulse::app {

// Keep contextual Git links alive until their exit finishes. Pinned links stay
// in the catalog and never enter this animation.
class SidebarProjectAnimation {
public:
    void Apply(ui::SidebarGroup& group, uint64_t now, bool animate) {
        if (group.hidden || group.collapsed) {
            tracks_.clear();
            initialized_ = true;
            return;
        }
        for (size_t i = 0; i < group.items.size(); ++i) {
            const auto& item = group.items[i];
            if (!item.transient_project) continue;
            auto found = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
                return SamePath(track.item, item);
            });
            if (found == tracks_.end()) {
                Track track;
                track.visibility.Tick(false, now, true, kDurationMs);
                tracks_.push_back(std::move(track));
                found = std::prev(tracks_.end());
            }
            found->item = item;
            found->index = i;
        }
        for (auto it = tracks_.begin(); it != tracks_.end();) {
            auto live = std::find_if(group.items.begin(), group.items.end(), [&](const auto& item) {
                return !item.exiting && SamePath(it->item, item);
            });
            // Pinning the same path replaces the contextual row immediately.
            if (live != group.items.end() && !live->transient_project) {
                it = tracks_.erase(it);
                continue;
            }
            const bool visible = live != group.items.end();
            it->visibility.Tick(visible, now, animate && initialized_, kDurationMs);
            if (visible) {
                live->presence = it->visibility.value;
            } else if (it->visibility.value > 0.0f) {
                auto ghost = it->item;
                ghost.exiting = true;
                ghost.presence = it->visibility.value;
                group.items.insert(group.items.begin() + static_cast<std::ptrdiff_t>(
                    std::min(it->index, group.items.size())), std::move(ghost));
            } else {
                it = tracks_.erase(it);
                continue;
            }
            ++it;
        }
        initialized_ = true;
    }

    bool Pending() const noexcept {
        return std::any_of(tracks_.begin(), tracks_.end(), [](const Track& track) {
            return track.visibility.Pending();
        });
    }

private:
    struct Track {
        ui::SidebarItem item;
        size_t index = 0;
        ui::BinaryTransition visibility;
    };
    static constexpr float kDurationMs = 180.0f;
    std::vector<Track> tracks_;
    bool initialized_ = false;

    static bool SamePath(const ui::SidebarItem& left, const ui::SidebarItem& right) {
        return _wcsicmp(left.path.c_str(), right.path.c_str()) == 0;
    }
};

} // namespace pulse::app
