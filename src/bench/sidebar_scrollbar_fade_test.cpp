#include "../app/sidebar_scrollbar_fade.h"
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
    fade.Tick(700, 30, true, false);
    check(fade.opacity == 1 && fade.expand == 1, "hover expands the draggable thumb");
    fade.Tick(1600, 30, false, true);
    check(fade.opacity == 1 && fade.expand == 1, "drag remains visible beyond the idle timeout");
    fade.Tick(1800, 30, false, false);
    check(fade.opacity == 0 && fade.expand == 0, "released inactive scrollbar fades out");
    check(!fade.Tick(1816, 30, false, false), "fully hidden state does not invalidate idle frames");
    fade.Tick(1900, 30, true, false);
    check(fade.opacity > 0 && fade.expand > 0, "hovering the invisible track reveals the thumb");
    return failures ? 1 : 0;
}
