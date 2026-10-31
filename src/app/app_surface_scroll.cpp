// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "app_surface_scroll.h"

namespace pulse {
void CancelAppSurfaceScroll(AppState& s) {
    s.sidebarScrollMotion.Reset();
    s.detailsScrollMotion.Reset();
    s.horizontalScrollMotion.Reset();
    s.horizontalScrollTab = nullptr;
    s.horizontalScrollPath.clear();
    ForEachPane(s, [](app::Pane& pane) {
        auto& strip = pane.view.column_strip;
        strip.horizontal_motion.Reset();
        for (auto& column : strip.ancestors) column.scroll_motion.Reset();
        strip.child.scroll_motion.Reset();
    });
}

void StartHorizontalSmoothScroll(AppState& s, float delta, float maximum, float viewport, uint64_t now) {
    auto* tab = ActiveTab(s);
    if (!tab) return;
    if (s.horizontalScrollTab != tab || s.horizontalScrollPath != tab->current_path) s.horizontalScrollMotion.Reset();
    s.horizontalScrollTab = tab;
    s.horizontalScrollPath = tab->current_path;
    s.horizontalScrollMotion.ScrollBy(tab->scroll_x, delta, maximum, viewport, now,
        s.renderer.MotionEnabled() && !s.shot_high_contrast);
}

bool TickAppSurfaceScroll(AppState& s, ui::WindowViewModel& vm, uint64_t now) {
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    const bool animate = s.renderer.MotionEnabled() && !s.shot_high_contrast;
    if (s.sidebarScrollMotion.Pending()) {
        if (s.renderer.EffectiveSidebarWidth(w) <= 60 * s.scale || s.scrollbarSidebar) s.sidebarScrollMotion.Reset();
        else s.sidebarScrollMotion.Tick(s.sidebarScroll, s.renderer.SidebarMaxScroll(vm, w, h), now, animate);
        vm.sidebar_scroll = s.sidebarScroll;
    }
    if (s.detailsScrollMotion.Pending()) {
        const auto rect = s.renderer.DetailsPanelRect(w, h);
        if (!s.showDetailsPanel || rect.right <= rect.left || IsSettingsTab(ActiveTab(s))) s.detailsScrollMotion.Reset();
        else {
            const float maximum = std::max(0.0f, s.renderer.DetailsContentHeightDip(vm, w, h) -
                (rect.bottom - rect.top) / s.scale);
            s.detailsScrollMotion.Tick(s.detailsScroll, maximum, now, animate);
            vm.details.scroll_y = s.detailsScroll;
        }
    }
    if (s.horizontalScrollMotion.Pending()) {
        auto* tab = ActiveTab(s);
        if (!tab || tab != s.horizontalScrollTab || tab->current_path != s.horizontalScrollPath ||
            IsSettingsTab(tab) || (s.scrollbarDragging && s.scrollbarHorizontal)) s.horizontalScrollMotion.Reset();
        else {
            s.horizontalScrollMotion.Tick(tab->scroll_x,
                s.renderer.MaxScrollXForPane(vm.pane, FocusedPaneRect(s)), now, animate);
            vm.pane.scroll_x = tab->scroll_x;
            for (auto& slot : vm.pane_slots) if (slot.focused) slot.pane.scroll_x = tab->scroll_x;
        }
    }
    return s.sidebarScrollMotion.Pending() || s.detailsScrollMotion.Pending() || s.horizontalScrollMotion.Pending();
}
}
