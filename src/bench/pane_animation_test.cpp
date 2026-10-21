#include "../app/pane_animations.h"
#include "../app/ui_schedule_state.h"
#include "../app/sidebar_scrollbar_fade.h"
#include <cstdio>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int main() {
    using namespace pulse::app;
    int failures = 0;
    auto check = [&](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    };
    UiDeadlinePlan deadlines;
    check(deadlines.Delay(100) == 0, "empty deadline plan schedules no timer");
    deadlines.Include(300);
    deadlines.Include(0);
    deadlines.Include(180);
    check(deadlines.due == 180 && deadlines.Delay(100) == 80, "shared timer selects the earliest live deadline");
    check(deadlines.Delay(200) == 10, "overdue tasks use a bounded Win32 wakeup instead of a zero-delay spin");
    deadlines.due = UINT64_MAX;
    check(deadlines.Delay(1) == 0x7fffffff, "long deadlines cannot overflow the timer interval");
    Pane pane;
    check(PaneAnimationsPending(pane, true, false), "new pane requests initial state synchronization");
    check(!TickPaneAnimations(pane, true, false, 1000) && pane.header_animation.opacity == 1,
          "initial focused header appears immediately and idle animation stops");
    check(PaneAnimationsPending(pane, true, true), "editing wakes a settled pane");
    check(TickPaneAnimations(pane, true, true, 1100) && pane.filter_expand == 0,
          "zero-elapsed opening frame schedules its successor");
    TickPaneAnimations(pane, true, true, 1104);
    const float first = pane.filter_expand;
    TickPaneAnimations(pane, true, true, 1108);
    check(first > 0 && pane.filter_expand > first, "filter advances on successive 4 ms display frames");
    check(!TickPaneAnimations(pane, true, true, 1240) && pane.filter_expand == 1,
          "filter reaches the endpoint in 140 ms and releases the frame loop");
    pane.view.filter_text = L"report";
    check(!PaneAnimationsPending(pane, true, false), "populated unfocused filter stays open without animation");
    pane.view.filter_text.clear();
    check(TickPaneAnimations(pane, false, false, 1300), "focus and filter changes animate together");
    TickPaneAnimations(pane, false, false, 1350);
    const float opacity = pane.header_animation.opacity;
    const float expansion = pane.filter_expand;
    check(TickPaneAnimations(pane, true, true, 1350) &&
          pane.header_animation.opacity == opacity && pane.filter_expand == expansion,
          "rapid reversal keeps both visible values continuous");
    TickPaneAnimations(pane, true, true, 1354);
    check(pane.header_animation.opacity > opacity && pane.filter_expand > expansion,
          "reversed animation advances on the next display frame");
    check(!TickPaneAnimations(pane, true, true, 5000) && pane.header_animation.opacity == 1 &&
          pane.filter_expand == 1, "long interruption settles without replaying missed frames");
    check(!TickPaneAnimations(pane, true, true, 5004), "settled values request no additional frames");
    Pane redirected;
    TickPaneAnimations(redirected, false, false, 1000);
    TickPaneAnimations(redirected, true, true, 1100);
    Pane sampled;
    TickPaneAnimations(sampled, false, false, 1000);
    TickPaneAnimations(sampled, true, true, 1100);
    TickPaneAnimations(sampled, true, true, 1170);
    check(TickPaneAnimations(redirected, false, false, 1170) &&
          redirected.header_animation.opacity > 0 && redirected.filter_expand > 0 &&
          redirected.header_animation.opacity == sampled.header_animation.opacity &&
          redirected.filter_expand == sampled.filter_expand,
          "retarget between display frames samples the old tracks at the new timestamp");
    TickPaneAnimations(redirected, false, false, 1180);
    check(redirected.header_animation.opacity < sampled.header_animation.opacity &&
          redirected.filter_expand < sampled.filter_expand,
          "retargeted tracks move from the current sample toward the new endpoint");
    TickPaneAnimations(redirected, true, true, 1300);
    check(!TickPaneAnimations(redirected, true, true, 1320, false) &&
          redirected.header_animation.opacity == 1 && redirected.filter_expand == 1 &&
          !PaneAnimationsPending(redirected, true, true),
          "turning off animation during a transition snaps both tracks and releases scheduling");
    check(!TickPaneAnimations(redirected, false, false, 1321, false) &&
          redirected.header_animation.opacity == 0 && redirected.filter_expand == 0,
          "disabled motion applies focus and filter endpoints in the same frame");
    check(!TickPaneAnimations(redirected, false, false, 1400),
          "re-enabling motion never replays the completed transition");
    redirected.view.filter_text = L"retained";
    check(!TickPaneAnimations(redirected, false, false, 1410, false) && redirected.filter_expand == 1,
          "disabled motion preserves a populated filter's open state");
    SidebarScrollbarFade fade;
    fade.Tick(1000, 40, false, false, false);
    check(fade.opacity == 1 && fade.expand == 0 && !fade.Pending() && fade.NextWake() == 1900,
          "disabled scrollbar motion shows immediately while retaining its auto-hide deadline");
    fade.Tick(1900, 40, false, false, false);
    check(fade.opacity == 0 && !fade.Pending() && !fade.NextWake(),
          "disabled scrollbar motion hides at its deadline without starting frame scheduling");
    fade.Tick(2000, 40, true, false, false);
    check(fade.opacity == 1 && fade.expand == 1 && !fade.Pending(),
          "disabled scrollbar motion preserves immediate hover feedback");
    for (uint64_t step : {4ull, 7ull, 16ull, 33ull}) {
        Pane sample;
        TickPaneAnimations(sample, false, false, 1000);
        bool active = TickPaneAnimations(sample, true, true, 1100);
        bool bounded = active;
        float previous_opacity = 0, previous_expansion = 0;
        uint64_t elapsed = 0;
        for (elapsed = step; elapsed <= 220 && active; elapsed += step) {
            active = TickPaneAnimations(sample, true, true, 1100 + elapsed);
            bounded &= sample.header_animation.opacity >= previous_opacity &&
                sample.header_animation.opacity <= 1 && sample.filter_expand >= previous_expansion &&
                sample.filter_expand <= 1;
            previous_opacity = sample.header_animation.opacity;
            previous_expansion = sample.filter_expand;
        }
        check(bounded && !active && sample.header_animation.opacity == 1 && sample.filter_expand == 1 &&
              elapsed - step >= 180 && elapsed - step < 180 + step,
              "30/60/144/240 Hz-like schedules preserve duration, bounds and termination");
    }
    return failures ? 1 : 0;
}
