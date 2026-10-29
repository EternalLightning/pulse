#pragma once
#include "../ui/ui_renderer.h"
#include "../ui/animation_clock.h"
#include <cmath>
#include <cstdio>

namespace pulse::ui {
struct SurfaceScrollUiTest {
    static int Run() {
        int failures = 0;
        auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        MainRenderer renderer;
        renderer.motion_enabled_ = true;
        renderer.SetScale(1.0f);
        WindowViewModel vm;
        vm.tabs.resize(24);
        vm.tabs.front().active = true;
        const auto strip = renderer.ComputeTabStrip(vm, 480.0f);
        const float distance = (strip.viewport.right - strip.viewport.left) * 0.75f * 0.125f;
        check(renderer.ScrollTabStrip(vm, 480.0f, 0.125f) && renderer.SurfaceScrollAnimating(),
            "tab wheel starts animation for a partial notch");
        const uint64_t tab_now = AnimationTimeMs();
        renderer.TickSurfaceScroll(tab_now + 40);
        check(renderer.tab_strip_scroll_.offset > 0.0f && renderer.tab_strip_scroll_.offset < distance,
            "tab wheel has an intermediate offset before reaching its endpoint");
        for (uint64_t elapsed = 56; elapsed <= 2000 && renderer.SurfaceScrollAnimating(); elapsed += 16)
            renderer.TickSurfaceScroll(tab_now + elapsed);
        check(!renderer.SurfaceScrollAnimating() &&
            std::abs(renderer.tab_strip_scroll_.offset - distance) <= 0.6f,
            "partial tab wheel reaches its proportional endpoint and stops scheduling");
        renderer.ScrollTabStrip(vm, 480.0f, 1.0f);
        vm.tabs.front().active = false;
        vm.tabs.back().active = true;
        renderer.ComputeTabStrip(vm, 480.0f);
        check(!renderer.SurfaceScrollAnimating(), "switching the active tab cancels old strip momentum");
        renderer.ScrollTabStrip(vm, 480.0f, -1.0f);
        vm.tab_drag_index = 0;
        renderer.ComputeTabStrip(vm, 480.0f);
        check(!renderer.SurfaceScrollAnimating(), "tab dragging cancels strip momentum");
        vm.tab_drag_index = -1;

        renderer.details_preview_ready_ = true;
        renderer.details_viewport_.SetContent(D2D1::RectF(0, 0, 200, 160), 1200, 2400, false);
        renderer.ScrollDetailsPreview(-1, 0, 0);
        renderer.ScrollDetailsPreview(1, 0, 0, true);
        renderer.TickSurfaceScroll(AnimationTimeMs() + 40);
        check(renderer.details_viewport_.x > 0 && renderer.details_viewport_.x < 48 &&
            renderer.details_viewport_.y > 0 && renderer.details_viewport_.y < 48,
            "text preview smooths both wheel axes with their existing direction");
        const float pan_x = renderer.details_viewport_.x, pan_y = renderer.details_viewport_.y;
        renderer.BeginDetailsPreviewPan(10, 10);
        renderer.TickSurfaceScroll(AnimationTimeMs() + 1000);
        check(!renderer.SurfaceScrollAnimating() && renderer.details_viewport_.x == pan_x &&
            renderer.details_viewport_.y == pan_y, "grab panning cancels both preview wheel axes");
        renderer.EndDetailsPreviewPan();
        renderer.motion_enabled_ = false;
        renderer.ScrollDetailsPreview(-1, 0, 0);
        check(!renderer.SurfaceScrollAnimating() && renderer.details_viewport_.y == pan_y + 48,
            "disabled motion applies the preview wheel endpoint immediately");
        renderer.motion_enabled_ = true;
        renderer.ScrollDetailsPreview(-1, 0, 0);
        renderer.details_viewport_.SetContent(D2D1::RectF(0, 0, 200, 160), 200, 160, false);
        renderer.TickSurfaceScroll(AnimationTimeMs() + 40);
        check(!renderer.SurfaceScrollAnimating() && renderer.details_viewport_.y == 0,
            "shrinking preview content clamps its offset and cancels momentum");
        renderer.details_viewport_.SetContent(D2D1::RectF(0, 0, 200, 160), 1200, 2400, false);
        renderer.ScrollDetailsPreview(-1, 0, 0);
        renderer.SetDetailsPanelVisible(false);
        check(!renderer.SurfaceScrollAnimating(), "hiding the details panel releases scroll scheduling");
        renderer.details_preview_ready_ = true;
        renderer.details_viewport_.SetContent(D2D1::RectF(0, 0, 200, 160), 1200, 2400, true);
        const float zoom = renderer.details_viewport_.zoom;
        renderer.ScrollDetailsPreview(1, 100, 80);
        check(renderer.details_viewport_.zoom > zoom && !renderer.SurfaceScrollAnimating(),
            "image wheel retains zoom behavior without starting a scroll animation");
        return failures ? 1 : 0;
    }
};
} // namespace pulse::ui
