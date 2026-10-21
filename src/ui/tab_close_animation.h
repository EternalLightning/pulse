#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace pulse::ui {

// Capture the displayed strip before deletion, then move surviving spans to
// their new layout. Positions and widths share one timeline, including chips
// and the plus button. The renderer also uses these spans for hit testing.
class TabCloseAnimation {
public:
    struct Item {
        const void* tab = nullptr;
        int group = -1; // null tab + -1 denotes the plus button
        float left = 0, right = 0;
        bool SameKey(const Item& other) const noexcept {
            return tab == other.tab && group == other.group;
        }
        bool operator==(const Item&) const = default;
    };

    void Capture(std::vector<Item> displayed, float window_width, float scale) {
        from_ = std::move(displayed);
        window_width_ = window_width;
        scale_ = scale;
        armed_ = true;
        pending_ = false;
    }

    void Apply(std::vector<Item>& layout, uint64_t now, float window_width,
               float scale, bool animate) {
        if (!animate || window_width != window_width_ || scale != scale_) {
            Reset();
            return;
        }
        if (armed_) {
            armed_ = false;
            target_ = layout;
            std::vector<Item> starts = layout;
            for (auto& item : starts) {
                const auto previous = std::find_if(from_.begin(), from_.end(),
                    [&](const Item& old) { return old.SameKey(item); });
                if (previous != from_.end()) item = *previous;
            }
            from_ = std::move(starts);
            started_ = now;
            pending_ = from_ != target_;
        }
        // Resize, scrolling, adding or reordering tabs invalidates the captured
        // layout. A consecutive close captures the current spans before this.
        if (!pending_ || layout != target_) {
            Reset();
            return;
        }
        const float t = std::clamp(static_cast<float>(now >= started_ ? now - started_ : 0)
            / 200.0f, 0.0f, 1.0f);
        if (t >= 1) { Reset(); return; }
        const float eased = t * t * (3.0f - 2.0f * t);
        for (size_t i = 0; i < layout.size(); ++i) {
            layout[i].left = from_[i].left + (target_[i].left - from_[i].left) * eased;
            layout[i].right = from_[i].right + (target_[i].right - from_[i].right) * eased;
        }
    }

    bool Pending() const noexcept { return pending_; }
    void Reset() noexcept {
        armed_ = pending_ = false;
        from_.clear();
        target_.clear();
    }

private:
    std::vector<Item> from_, target_;
    float window_width_ = 0, scale_ = 0;
    uint64_t started_ = 0;
    bool armed_ = false, pending_ = false;
};
} // namespace pulse::ui
