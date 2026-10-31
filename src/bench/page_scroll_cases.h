// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_surface_scroll.h"
#include "../ui/animation_clock.h"
#include "smooth_scroll_cases.h"
#include <cmath>

inline bool RunPageScrollChecks(pulse::AppState& s) {
    using namespace pulse;
    int failures = RunSmoothScrollChecks();
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    s.compositor.Resize(1100, 700);
    for (int i = 0; i < 60; ++i) {
        app::SidebarEntry entry;
        entry.label = L"Scroll fixture " + std::to_wstring(i);
        entry.path = L"C:\\PulseScrollFixture\\" + std::to_wstring(i);
        s.sidebar.quick_access.push_back(std::move(entry));
    }
    const auto wheel = [&](const D2D1_RECT_F& rect, short delta) {
        POINT point{static_cast<LONG>(rect.left + 20), static_cast<LONG>(rect.top + 40)};
        ClientToScreen(s.hwnd, &point);
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(point.x, point.y));
    };
    for (bool settings : {false, true}) {
        if (settings) OpenSettingsTab(s, 0);
        s.sidebarScroll = 0;
        s.sidebarScrollMotion.Reset();
        auto vm = BuildVm(s, false);
        const auto rect = s.renderer.SidebarRect(1100, 700);
        check(s.renderer.SidebarMaxScroll(vm, 1100, 700) > 0, "sidebar fixture overflows");
        wheel(rect, -WHEEL_DELTA / 4);
        check(s.sidebarScroll == 0 && s.sidebarScrollMotion.Pending(),
            settings ? "settings routes sidebar wheel to its independent animation" : "sidebar accepts a partial wheel notch smoothly");
        const auto now = ui::AnimationTimeMs();
        TickAppSurfaceScroll(s, vm, now + 32);
        check(s.sidebarScroll > 0 && s.sidebarScroll < 12, "sidebar advances through an intermediate frame");
        for (uint64_t elapsed = 48; s.sidebarScrollMotion.Pending() && elapsed < 2000; elapsed += 16)
            TickAppSurfaceScroll(s, vm, now + elapsed);
        check(!s.sidebarScrollMotion.Pending() && std::abs(s.sidebarScroll - 12) < 0.6f,
            "sidebar partial wheel settles and releases frame scheduling");
        check(!s.scrollAnimating, "sidebar scroll does not move the page underneath");
        s.shot_high_contrast = true;
        wheel(rect, -WHEEL_DELTA);
        check(!s.sidebarScrollMotion.Pending() && std::abs(s.sidebarScroll - 60) < 0.6f,
            "high contrast sidebar reaches its endpoint immediately");
        s.shot_high_contrast = false;
    }
    ActiveTab(s)->current_path = L"C:\\PulseScrollFixture";
    auto& tab = *ActiveTab(s);
    s.horizontalScrollMotion.ScrollBy(tab.scroll_x, 50, 1000, 400, 1000, true);
    s.horizontalScrollTab = &tab;
    s.horizontalScrollPath = L"C:\\OldFolder";
    auto vm = BuildVm(s, false);
    TickAppSurfaceScroll(s, vm, 1016);
    check(!s.horizontalScrollMotion.Pending(), "navigation cancels horizontal momentum from the old folder");
    tab.column_strip.horizontal_motion.ScrollBy(tab.column_strip.scroll_from_right_dip,
        50, 1000, 400, 2000, true);
    tab.column_strip.child.scroll_motion.ScrollBy(tab.column_strip.child.scroll_dip,
        50, 1000, 400, 2000, true);
    BindCurrentLayout(s);
    check(!tab.column_strip.horizontal_motion.Pending() && !tab.column_strip.child.scroll_motion.Pending(),
        "switching layouts cancels column motion before hidden tabs can resume it");
    return failures == 0;
}
