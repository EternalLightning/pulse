// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_hover.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "app_change_tracking.h"

namespace pulse {
bool UpdateHoverFromHit(AppState& s, const ui::HitTestResult& hit, POINT point) {
    const bool target_changed = s.hoverRegion != static_cast<int>(hit.region) ||
        s.hoverControlIndex != hit.index || s.hoverSubIndex != hit.sub_index ||
        s.hoverPaneIndex != hit.pane_index || s.hoverPath != hit.path || s.hoverLabel != hit.label;
    if (target_changed) ApplyHoverTarget(s, hit);
    using R = ui::HitTestResult;
    const int row = hit.region == R::Row || hit.region == R::RowStar || hit.region == R::RowFolderSize ||
        hit.region == R::RowNewTab || hit.region == R::ChangeBadge || hit.region == R::RowMore ? hit.index : -1;
    const int crumb = hit.region == R::BreadcrumbSegment ? hit.index : -1;
    const bool row_changed = s.hoverRow != row || s.hoverPaneIndex != hit.pane_index;
    const bool changed = target_changed || row_changed || s.breadcrumbHover != crumb;
    if (row_changed) s.ctxHoverSince = GetTickCount64();
    s.hoverRow = row;
    s.hoverPaneIndex = hit.pane_index;
    s.breadcrumbHover = crumb;
    UpdateChangeHover(s, hit, point);
    return changed;
}

void RefreshStationaryHover(AppState& s, ui::WindowViewModel& vm) {
    if (!s.mouseLeaveTracked || GetCapture() == s.hwnd) return;
    const auto rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    const auto hit = s.renderer.HitTest(vm, rect,
        static_cast<float>(s.hoverPoint.x), static_cast<float>(s.hoverPoint.y));
    UpdateHoverFromHit(s, hit, s.hoverPoint);
    vm.hover_region = s.hoverRegion;
    vm.hover_control_index = s.hoverControlIndex;
    vm.hover_sub_index = s.hoverSubIndex;
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.breadcrumb_hover = s.breadcrumbHover;
    vm.tooltip_text = s.tooltipText;
    vm.pane.hover_index = -1;
    for (size_t i = 0; i < vm.pane_slots.size(); ++i) {
        auto& slot = vm.pane_slots[i];
        slot.pane.hover_index = static_cast<int>(i) == s.hoverPaneIndex ? s.hoverRow : -1;
        if (slot.focused) vm.pane.hover_index = slot.pane.hover_index;
    }
    if (vm.pane_slots.empty()) vm.pane.hover_index = s.hoverRow;
}
}
