// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pulse::index {
// Retain totals for directories only. File metadata stays in the existing MFT
// index; an edit touches just the old and new ancestor chains.
class FolderSizeIndex {
public:
    struct Item { int32_t parent = -1; uint64_t bytes = 0; bool directory = false, alive = false; };
    void Reset() { valid_ = false; totals_.clear(); }
    bool Valid() const { return valid_; }
    uint64_t Builds() const { return builds_; }
    std::optional<uint64_t> Get(int32_t id) const {
        const auto it = totals_.find(id);
        return valid_ && it != totals_.end() ? std::optional(it->second) : std::nullopt;
    }
    template<class Read> bool Build(int32_t count, Read read) {
        Reset(); ++builds_;
        std::vector<uint64_t> sums(static_cast<size_t>(count));
        std::vector<uint32_t> children(static_cast<size_t>(count));
        std::vector<int32_t> queue;
        queue.reserve(static_cast<size_t>(count));
        size_t live = 0;
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive) continue;
            ++live; sums[i] = n.directory ? 0 : n.bytes;
            if (n.parent >= count || n.parent == i) return false;
            if (n.parent >= 0 && read(n.parent).alive) {
                if (!read(n.parent).directory) return false;
                ++children[n.parent];
            }
        }
        for (int32_t i = 0; i < count; ++i) if (read(i).alive && !children[i]) queue.push_back(i);
        for (size_t at = 0; at < queue.size(); ++at) {
            const auto i = queue[at]; const auto n = read(i);
            if (n.directory) totals_[i] = sums[i];
            if (n.parent >= 0 && read(n.parent).alive) {
                if (UINT64_MAX - sums[n.parent] < sums[i]) { Reset(); return false; }
                sums[n.parent] += sums[i];
                if (--children[n.parent] == 0) queue.push_back(n.parent);
            }
        }
        valid_ = queue.size() == live;
        if (!valid_) totals_.clear();
        return valid_;
    }
    template<class Read> void Replace(int32_t id, Item before, Item after, Read read) {
        if (!valid_) return;
        if (before.alive && after.alive && before.directory != after.directory) { Reset(); return; }
        const uint64_t old_size = before.directory ? Get(id).value_or(0) : before.bytes;
        const uint64_t new_size = after.directory ? (before.alive ? old_size : 0) : after.bytes;
        auto adjust = [&](int32_t parent, uint64_t bytes, bool add) {
            // Malformed chains must never hang the index service.
            for (size_t depth = 0; parent >= 0; ++depth) {
                if (depth > totals_.size()) return false;
                const auto n = read(parent);
                if (!n.alive) return true;
                const auto it = totals_.find(parent);
                if (it == totals_.end()) return false;
                auto& total = it->second;
                if (add ? UINT64_MAX - total < bytes : total < bytes) return false;
                if (add) total += bytes; else total -= bytes;
                parent = n.parent;
            }
            return true;
        };
        if ((before.alive && !adjust(before.parent, old_size, false)) ||
            (after.alive && !adjust(after.parent, new_size, true))) { Reset(); return; }
        if (after.alive && after.directory) totals_[id] = new_size;
        else totals_.erase(id);
    }
private:
    bool valid_ = false;
    uint64_t builds_ = 0;
    std::unordered_map<int32_t, uint64_t> totals_;
};
}
