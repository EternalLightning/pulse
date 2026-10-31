// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "item_reflow_animation.h"
#include <string_view>

namespace pulse::ui {

// Sample identity lookups once per frame. Vacancy checks then inspect only
// contiguous geometry, and foreground/background share the same reveal clip.
class ItemReflowGeometry {
public:
    ItemReflowGeometry() = default;
    ItemReflowGeometry(const ItemReflowGeometry&) = delete;
    ItemReflowGeometry& operator=(const ItemReflowGeometry&) = delete;

    void Reset() {
        indices_.clear();
        entries_.clear();
    }

    void Update(const std::vector<ItemReflowSlot>& slots, const ItemReflowAnimation& animation) {
        indices_.clear();
        entries_.resize(slots.size());
        indices_.reserve(slots.size());
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& entry = entries_[i];
            entry.key = slots[i].key;
            entry.bounds = slots[i].bounds;
            entry.visual = animation.Visual(slots[i].key);
            entry.vacancy_valid = false;
            entry.identity = indices_.emplace(entry.key, i).first->second;
        }
    }

    D2D1_RECT_F Vacancy(const std::wstring& key, D2D1_RECT_F bounds) const {
        const auto found = indices_.find(key);
        const size_t identity = found == indices_.end() ? entries_.size() : found->second;
        const Entry* cached = identity < entries_.size() ? &entries_[identity] : nullptr;
        const bool cache = cached && Equal(cached->bounds, bounds);
        if (cache && cached->vacancy_valid) return cached->vacancy;
        auto available = bounds;
        for (const auto& entry : entries_) {
            if (entry.identity == identity) continue;
            const auto& other = entry.visual;
            if (other.opacity <= 0 || other.exiting) continue;
            const bool same_column = entry.bounds.right > bounds.left && entry.bounds.left < bounds.right;
            const bool same_row = entry.bounds.bottom > bounds.top && entry.bounds.top < bounds.bottom;
            if (same_column && entry.bounds.top >= bounds.bottom)
                available.bottom = (std::min)(available.bottom, other.bounds.top);
            if (same_column && entry.bounds.bottom <= bounds.top)
                available.top = (std::max)(available.top, other.bounds.bottom);
            if (same_row && entry.bounds.left >= bounds.right)
                available.right = (std::min)(available.right, other.bounds.left);
            if (same_row && entry.bounds.right <= bounds.left)
                available.left = (std::max)(available.left, other.bounds.right);
        }
        if (cache) {
            cached->vacancy = available;
            cached->vacancy_valid = true;
        }
        return available;
    }

    D2D1_RECT_F RemovedVacancy(D2D1_RECT_F bounds) const {
        auto vacancy = bounds;
        for (const auto& entry : entries_) {
            const auto& other = entry.visual;
            if (other.opacity <= 0 || other.bounds.right <= bounds.left ||
                other.bounds.left >= bounds.right) continue;
            if (other.bounds.top >= bounds.top)
                vacancy.bottom = (std::min)(vacancy.bottom, other.bounds.top);
            else if (other.bounds.bottom <= bounds.bottom)
                vacancy.top = (std::max)(vacancy.top, other.bounds.bottom);
            else vacancy.bottom = vacancy.top;
        }
        return vacancy;
    }

private:
    struct Entry {
        std::wstring key;
        D2D1_RECT_F bounds{};
        ItemReflowVisual visual;
        size_t identity = 0;
        mutable D2D1_RECT_F vacancy{};
        mutable bool vacancy_valid = false;
    };
    static bool Equal(D2D1_RECT_F a, D2D1_RECT_F b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    std::vector<Entry> entries_;
    // Views refer to owned entry keys, which stay fixed until the next Update.
    std::unordered_map<std::wstring_view, size_t> indices_;
};

} // namespace pulse::ui
