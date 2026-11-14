// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/column_strip_layout.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    check(ColumnStripWidthSlot(2, 3) == 1 && ColumnStripWidthSlot(0, 3) == 3 &&
          ColumnStripWidthSlot(kColumnStripChildId, 3) == 0, "widths follow ancestor depth and child identity");
    check(ColumnStripWidthDip({1.0f, 900.0f}, 0) == kColumnStripMinDip &&
          ColumnStripWidthDip({1.0f, 900.0f}, 1) == kColumnStripMaxDip, "stored widths respect limits");
    for (float scale : {1.0f, 1.5f, 2.0f}) for (float width : {640.0f, 1200.0f}) {
        for (bool child : {false, true}) for (float parent_width : {160.0f, 280.0f}) {
            const auto anchored = LayoutColumnStrip(D2D1::RectF(10, 20, 10 + width * scale, 600 * scale),
                80 * scale, 6, child, {220.0f, parent_width}, scale, 0, -30.0f);
            const auto parent = std::find_if(anchored.columns.begin(), anchored.columns.end(),
                [](const auto& col) { return col.id == 5; });
            check(parent != anchored.columns.end() && std::abs(parent->rect.left - (10 - 30 * scale)) < .01f,
                "clicked parent keeps its x across widths, DPI, column resize and asynchronous child visibility");
            check(anchored.body.right - anchored.body.left >= kColumnStripCurrentMinDip * scale - .01f,
                "anchored parent preserves the current list minimum width");
        }
    }
    struct Scenario { float scale; float width; };
    for (const auto [scale, width] : {Scenario{1, 200}, {1, 360}, {1, 640}, {1, 1200},
        {1.25f, 640}, {1.5f, 640}, {2, 640}}) {
        const auto bounds = D2D1::RectF(10, 20, 10 + width * scale, 600 * scale);
        const auto layout = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale);
        check(layout.body.left >= bounds.left && layout.body.right <= bounds.right &&
              layout.body.right >= layout.body.left, "current pane remains inside bounds across DPI and narrow widths");
        check(!layout.active || layout.body.right - layout.body.left >= kColumnStripCurrentMinDip * scale - .01f,
              "ancestors and child preserve usable current-folder width");
        check(std::abs(layout.scroll_px - layout.max_scroll_px) < .01f,
              "initial ancestor viewport stays beside immediate parent");
        const auto scrolled = LayoutColumnStrip(bounds, 80 * scale, 12, true, {}, scale, 100000);
        check(scrolled.scroll_px == 0, "horizontal overscroll clamps at oldest ancestor");
        const auto empty = LayoutColumnStrip(bounds, 80 * scale, 0, false, {}, scale);
        check(!empty.active && empty.body.left == bounds.left && empty.body.right == bounds.right,
              "empty strip restores full pane width");
    }
    return failures ? 1 : 0;
}
