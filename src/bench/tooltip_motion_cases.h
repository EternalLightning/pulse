// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ui/ui_renderer.h"
#include <cstdio>

namespace pulse::ui {
struct TooltipMotionUiTest {
    static int Run() {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        MainRenderer renderer;
        WindowViewModel vm;
        const auto tick = [&](uint64_t now, bool animate = true) {
            renderer.surface_animating_ = false;
            renderer.UpdateTooltipAnimation(vm, now, animate);
        };
        tick(1000);
        check(renderer.tooltip_text_.empty() && !renderer.SurfaceAnimating(),
            "empty tooltip starts without scheduling animation frames");
        vm.tooltip_text = L"First tooltip";
        vm.tooltip_x = 100;
        vm.tooltip_y = 200;
        vm.hover_region = HitTestResult::StatusBarCancelSearch;
        vm.hover_control_index = 1;
        tick(1100);
        check(renderer.tooltip_text_ == vm.tooltip_text && renderer.tooltip_transition_.value == 0 &&
            renderer.SurfaceAnimating(), "first tooltip starts at zero opacity and requests another frame");
        tick(1150);
        check(renderer.tooltip_transition_.value > 0 && renderer.tooltip_transition_.value < 1 &&
            renderer.SurfaceAnimating(), "tooltip has a visible intermediate fade-in frame");
        tick(1200);
        check(renderer.tooltip_transition_.value == 1 && !renderer.SurfaceAnimating(),
            "tooltip fade-in settles and releases animation scheduling");
        vm.tooltip_text.clear();
        vm.tooltip_x = 700;
        vm.tooltip_y = 800;
        vm.hover_region = 0;
        tick(1250);
        check(renderer.tooltip_transition_.value == 1 && renderer.tooltip_text_ == L"First tooltip" &&
            renderer.tooltip_x_ == 100 && renderer.tooltip_y_ == 200 &&
            renderer.tooltip_hover_region_ == HitTestResult::StatusBarCancelSearch && renderer.SurfaceAnimating(),
            "cleared tooltip retains its label, placement and status anchor while fading out");
        tick(1310);
        check(renderer.tooltip_transition_.value > 0 && renderer.tooltip_transition_.value < 1,
            "tooltip has a visible intermediate fade-out frame");
        tick(1370);
        check(renderer.tooltip_text_.empty() && renderer.tooltip_transition_.value == 0 &&
            !renderer.SurfaceAnimating(), "completed fade-out clears retained text and scheduling");
        vm.tooltip_text = L"Second tooltip";
        tick(1400);
        tick(1500);
        vm.tooltip_text = L"Replacement tooltip";
        tick(1510);
        check(renderer.tooltip_text_ == L"Replacement tooltip" && renderer.tooltip_transition_.value == 0 &&
            renderer.SurfaceAnimating(), "changing targets replaces old text before fading in its replacement");
        tick(1560);
        vm.hover_control_index = 2;
        tick(1561);
        check(renderer.tooltip_transition_.value == 0 && renderer.SurfaceAnimating(),
            "equal labels on different controls start a new tooltip instead of inheriting the old one");
        tick(1661);
        vm.tooltip_text.clear();
        tick(1700);
        tick(1760);
        const float fading = renderer.tooltip_transition_.value;
        vm.tooltip_text = L"Replacement tooltip";
        tick(1760);
        check(renderer.tooltip_transition_.value == fading && renderer.SurfaceAnimating(),
            "returning to the same tooltip reverses fade-out from its current opacity");
        tick(1790);
        const float entering = renderer.tooltip_transition_.value;
        vm.tooltip_text.clear();
        tick(1790);
        check(entering > fading && entering < 1 && renderer.tooltip_transition_.value == entering &&
            renderer.SurfaceAnimating(), "dismissing an entering tooltip reverses without an opacity jump");
        vm.tooltip_text = L"Replacement tooltip";
        tick(1790);
        tick(1800, false);
        check(renderer.tooltip_transition_.value == 1 && !renderer.SurfaceAnimating(),
            "disabled motion and high-contrast gate snap tooltip visibility immediately");
        vm.tooltip_text.clear();
        tick(1801, false);
        check(renderer.tooltip_text_.empty() && !renderer.SurfaceAnimating(),
            "disabled motion clears tooltip text immediately on dismissal");
        vm.tooltip_text = L"Suppressed tooltip";
        tick(1900, false);
        vm.change_popover.visible = true;
        tick(1901);
        check(renderer.tooltip_text_.empty() && !renderer.SurfaceAnimating(),
            "popover suppresses retained tooltip immediately");
        vm.change_popover.visible = false;
        tick(2000, false);
        vm.tab_drag_index = 0;
        tick(2001);
        check(renderer.tooltip_text_.empty() && !renderer.SurfaceAnimating(),
            "dragging immediately drops tooltip visuals and their animation work");
        return failures ? 1 : 0;
    }
};
} // namespace pulse::ui

inline int RunTooltipMotionChecks() {
    return pulse::ui::TooltipMotionUiTest::Run();
}
