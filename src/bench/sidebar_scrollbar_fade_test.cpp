#include "../app/sidebar_scrollbar_fade.h"
#include "../app/app_visual_animation.h"
#include <iostream>

int main() {
    pulse::app::SidebarScrollbarFade fade;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    check(!fade.Tick(100, 0, false, false) && fade.opacity == 0, "idle scrollbar stays hidden without scheduling frames");
    fade.Tick(116, 30, false, false); fade.Tick(216, 30, false, false);
    check(fade.opacity == 1 && fade.expand == 0, "scroll activity reveals a thin scrollbar");
    check(!fade.Pending() && fade.NextWake() == 1016, "visible hold waits for its deadline without continuous frames");
    fade.Tick(700, 30, true, false);
    fade.Tick(800, 30, true, false);
    check(fade.opacity == 1 && fade.expand == 1, "hover expands the draggable thumb");
    fade.Tick(1600, 30, false, true);
    check(fade.opacity == 1 && fade.expand == 1, "drag remains visible beyond the idle timeout");
    fade.Tick(1800, 30, false, false);
    check(fade.Pending() && fade.opacity == 1, "idle time before release does not skip the fade");
    fade.Tick(2000, 30, false, false);
    check(fade.opacity == 0 && fade.expand == 0, "released inactive scrollbar fades out");
    check(!fade.Tick(2016, 30, false, false) && !fade.Pending() && !fade.NextWake(), "fully hidden state does not invalidate idle frames");
    fade.Tick(2100, 30, true, false);
    fade.Tick(2100.5, 30, true, false);
    check(fade.opacity > 0 && fade.expand > 0, "hovering the invisible track reveals the thumb");
    check(fade.opacity < 0.01f, "fractional millisecond sampling advances without a whole frame jump");
    pulse::app::SidebarScrollbarFade deadline;
    deadline.Tick(100, 20, false, false);
    deadline.Tick(200, 20, false, false);
    deadline.Tick(1000, 20, false, false);
    check(deadline.Pending() && deadline.opacity == 1 && !deadline.NextWake(), "auto-hide deadline starts an animated fade after dormant hold");
    deadline.Tick(1100, 20, false, false);
    check(std::abs(deadline.opacity - 0.5f) < 0.001f, "auto-hide progresses over its intended duration");
    float low_rate = 0, high_rate = 0;
    for (int i = 0; i < 6; ++i) low_rate = pulse::app::AdvanceScrollbarHover(low_rate, 20, 16);
    for (int i = 0; i < 24; ++i) high_rate = pulse::app::AdvanceScrollbarHover(high_rate, 20, 4);
    check(std::abs(low_rate - high_rate) < 0.0001f, "scrollbar hover speed is independent of frame rate");
    return failures ? 1 : 0;
}
