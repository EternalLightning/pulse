#include "../app/pane_animations.h"
#include "../app/ui_schedule_state.h"
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
