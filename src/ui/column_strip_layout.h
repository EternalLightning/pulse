// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// column_strip_layout.h — Pure horizontal geometry of the column (Miller) view.
#pragma once

#include <d2d1.h>
#include <cstddef>
#include <vector>

namespace pulse::ui {

inline constexpr float kColumnStripDefaultDip = 200.0f;
inline constexpr float kColumnStripChildDefaultDip = 220.0f;
inline constexpr float kColumnStripMinDip = 120.0f;
inline constexpr float kColumnStripMaxDip = 560.0f;
// The current folder keeps at least this much room.
inline constexpr float kColumnStripCurrentMinDip = 240.0f;
// Largest fraction of the pane (after the child column) the ancestors use.
inline constexpr float kColumnStripAncestorShare = 0.5f;
// Column id of the trailing "contents of the selected folder" column.
// Ancestor columns use their index into the root-to-parent chain.
inline constexpr int kColumnStripChildId = 10000;

// Width slot 0 is the child column, slot k (k >= 1) the k-th ancestor counted
// from the current folder (1 = parent). Missing/zero entries use defaults.
float ColumnStripWidthDip(const std::vector<float>& widths_dip, size_t slot) noexcept;
size_t ColumnStripWidthSlot(int column_id, size_t ancestor_count) noexcept;

struct ColumnStripSlot {
    int id = -1;
    D2D1_RECT_F rect{};
};

struct ColumnStripLayout {
    bool active = false;
    // Replaces the pane bounds for everything the regular list draws:
    // same top/bottom as the pane, narrowed horizontally.
    D2D1_RECT_F body{};
    // Area the ancestor columns scroll inside (empty when there are none).
    D2D1_RECT_F viewport{};
    float scroll_px = 0.0f;      // from the left end of the ancestor strip
    float max_scroll_px = 0.0f;  // > 0: ancestors overflow the viewport
    float content_px = 0.0f;     // total ancestor width
    // Left to right: ancestors intersecting the viewport (rects may extend
    // past it; clip to viewport), then the child column when present.
    std::vector<ColumnStripSlot> columns;
};

// pane_bounds is the full pane rect; column rects span [top, pane bottom).
// Ancestors never drop out: when they do not fit they scroll horizontally.
// scroll_from_right_dip = 0 keeps the parent visible next to the list.
ColumnStripLayout LayoutColumnStrip(const D2D1_RECT_F& pane_bounds, float top,
                                    size_t ancestor_count, bool child_slot,
                                    const std::vector<float>& widths_dip,
                                    float scale, float scroll_from_right_dip = 0.0f) noexcept;

} // namespace pulse::ui
