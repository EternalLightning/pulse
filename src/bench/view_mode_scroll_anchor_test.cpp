// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/view_mode_scroll_anchor.h"
#include "../ui/view_layout.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}

bool Intersects(D2D1_RECT_F a, D2D1_RECT_F b) {
    return a.right > b.left && a.left < b.right && a.bottom > b.top && a.top < b.bottom;
}
}

int main() {
    using pulse::model::ViewMode;
    using pulse::ui::ViewLayout;
    constexpr D2D1_RECT_F viewport{320.0f, 200.0f, 1240.0f, 740.0f};
    constexpr int count = 2000;
    constexpr int selected = 1000;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (int old_mode = 0; old_mode < 8; ++old_mode) {
            const auto old = static_cast<ViewMode>(old_mode);
            const ViewLayout initial(old, viewport, count, 0, 0, scale);
            const pulse::app::ViewModeScrollAnchor desired{selected, 0.45f};
            const auto old_scroll = pulse::app::RestoreViewModeScrollAnchor(desired, old, viewport,
                initial.ItemRect(selected), initial.MaxScrollX(), initial.MaxScrollY());
            const ViewLayout before(old, viewport, count, old_scroll.x, old_scroll.y, scale);
            const auto capture = [&](int selected_view) {
                return pulse::app::CaptureViewModeScrollAnchor(old, viewport, selected_view,
                    before.VisibleRange(), [&](int i) { return before.ItemRect(i); }, [](int i) { return i; });
            };
            const auto anchor = capture(selected);
            Check(anchor.source_index == selected, "Visible selection takes precedence over first visible item");
            const auto fallback = capture(0);
            int first_visible = -1;
            const auto range = before.VisibleRange();
            for (int i = range.first; i <= range.second; ++i) {
                if (Intersects(before.ItemRect(i), viewport)) { first_visible = i; break; }
            }
            Check(fallback.source_index == first_visible && first_visible > 0,
                "Offscreen selection anchors first visible item and excludes overscan");
            for (int new_mode = 0; new_mode < 8; ++new_mode) {
                const auto mode = static_cast<ViewMode>(new_mode);
                const ViewLayout target(mode, viewport, count, 0, 0, scale);
                bool visible = true;
                for (const auto& saved : {anchor, fallback}) {
                    const auto scroll = pulse::app::RestoreViewModeScrollAnchor(saved, mode, viewport,
                        target.ItemRect(saved.source_index), target.MaxScrollX(), target.MaxScrollY());
                    const ViewLayout after(mode, viewport, count, scroll.x, scroll.y, scale);
                    const auto rect = after.ItemRect(saved.source_index);
                    visible = visible && Intersects(rect, viewport) && scroll.x >= 0 && scroll.y >= 0 &&
                        scroll.x <= target.MaxScrollX() && scroll.y <= target.MaxScrollY();
                    const bool horizontal = mode == ViewMode::List;
                    const float start = horizontal ? rect.left - viewport.left : rect.top - viewport.top;
                    const float travel = horizontal ? viewport.right - viewport.left - (rect.right - rect.left)
                                                    : viewport.bottom - viewport.top - (rect.bottom - rect.top);
                    visible = visible && std::abs(start - saved.position * std::max(0.0f, travel)) < 0.1f;
                    if (mode != ViewMode::List) visible = visible && scroll.x == 0.0f;
                    else visible = visible && scroll.y == 0.0f;
                }
                Check(visible, "Mode transition preserves anchor visibility, placement, and valid active-axis scroll");
            }
        }
    }
    const ViewLayout empty(ViewMode::Details, viewport, 0, 0, 0, 1);
    const auto missing = pulse::app::CaptureViewModeScrollAnchor(ViewMode::Details, viewport, -1,
        empty.VisibleRange(), [&](int i) { return empty.ItemRect(i); }, [](int i) { return i; });
    Check(missing.source_index == -1, "Empty listing has no anchor");

    const ViewLayout filtered(ViewMode::Details, viewport, 8, 0, 28.0f, 1);
    const std::vector<int> sources{400, 50, 900, 7, 205, 808, 19, 77};
    const auto mapped = pulse::app::CaptureViewModeScrollAnchor(ViewMode::Details, viewport, 3,
        filtered.VisibleRange(), [&](int i) { return filtered.ItemRect(i); },
        [&](int i) { return sources[static_cast<size_t>(i)]; });
    Check(mapped.source_index == 7, "Anchor retains source identity through nonmonotonic view mapping");
    const auto no_selection = pulse::app::CaptureViewModeScrollAnchor(ViewMode::Details, viewport, -1,
        filtered.VisibleRange(), [&](int i) { return filtered.ItemRect(i); },
        [&](int i) { return sources[static_cast<size_t>(i)]; });
    Check(no_selection.source_index == 50, "Item ending exactly at viewport top is excluded");

    const std::vector<pulse::ui::DateGroup> groups{{0, 40}, {40, 40}, {80, 40}};
    const ViewLayout grouped(ViewMode::Details, viewport, 120, 0, 1400.0f, 1, 28, &groups);
    const auto grouped_anchor = pulse::app::CaptureViewModeScrollAnchor(ViewMode::Details, viewport, -1,
        grouped.VisibleRange(), [&](int i) { return grouped.ItemRect(i); }, [](int i) { return i; });
    Check(grouped_anchor.source_index >= 0 && Intersects(grouped.ItemRect(grouped_anchor.source_index), viewport),
        "Grouped details anchor accounts for group headers");
    for (const auto mode : {ViewMode::Details, ViewMode::List}) {
        const ViewLayout target(mode, viewport, count, 0, 0, 1);
        const pulse::app::ViewModeScrollAnchor end{count - 1, 0.0f};
        const auto scroll = pulse::app::RestoreViewModeScrollAnchor(end, mode, viewport,
            target.ItemRect(count - 1), target.MaxScrollX(), target.MaxScrollY());
        const ViewLayout restored(mode, viewport, count, scroll.x, scroll.y, 1);
        Check(Intersects(restored.ItemRect(count - 1), viewport) &&
            (mode == ViewMode::List ? scroll.x == target.MaxScrollX() : scroll.y == target.MaxScrollY()),
            "End-of-list anchor clamps scroll and remains visible");
    }
    return failures ? 1 : 0;
}
