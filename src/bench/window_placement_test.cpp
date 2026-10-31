// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app/window_placement.h"
#include <iostream>
#include <limits>

static int failures = 0;
static void Check(const RECT& saved, const RECT& work, const RECT& expected, const char* name) {
    const auto actual = pulse::app::ConstrainRestoredWindowRect(saved, work);
    const bool pass = EqualRect(&actual, &expected) != FALSE;
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << name << std::endl;
    if (!pass) ++failures;
}
int main() {
    const RECT work{0, 0, 1920, 1040};
    Check({100, 100, 1100, 800}, work, {100, 100, 1100, 800}, "visible rectangle preserved");
    Check({2500, 200, 3500, 900}, work, {920, 200, 1920, 900}, "removed right monitor");
    Check({-1800, 100, -800, 800}, work, {0, 100, 1000, 800}, "removed left monitor");
    Check({100, -1600, 1100, -900}, work, {100, 0, 1100, 700}, "removed upper monitor");
    Check({-200, -100, 3000, 2000}, work, work, "oversized rectangle shrinks to work area");
    Check({-1800, 40, -800, 740}, {-1920, 40, 0, 1080}, {-1800, 40, -800, 740}, "negative-coordinate monitor retained");
    Check({100, 0, 1100, 700}, {0, 40, 1920, 1080}, {100, 40, 1100, 740}, "top taskbar work area respected");
    Check({0, 0, 0, 0}, work, {0, 0, 0, 0}, "empty session rectangle left for defaults");
    Check({std::numeric_limits<LONG>::min(), 0, std::numeric_limits<LONG>::max(), 1000}, work,
        {0, 0, 1920, 1000}, "malformed extreme coordinates do not overflow");
    const RECT missing{30000, 30000, 31000, 30700};
    const auto restored = pulse::app::ConstrainRestoredWindowRect(missing);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const bool pass = GetMonitorInfoW(MonitorFromRect(&restored, MONITOR_DEFAULTTONULL), &info) &&
        restored.left >= info.rcWork.left && restored.top >= info.rcWork.top &&
        restored.right <= info.rcWork.right && restored.bottom <= info.rcWork.bottom;
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << "actual monitor lookup constrains missing-monitor rectangle" << std::endl;
    if (!pass) ++failures;
    std::cout << "RESULT failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
