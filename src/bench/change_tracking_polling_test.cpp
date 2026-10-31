// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/change_tracking_polling.h"
#include <cstdio>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    using pulse::app::ShouldPollChangeTracking;
    check(!ShouldPollChangeTracking(nullptr), "unavailable window does not poll");
    // Off-screen, non-activating fixture; never touches the user's Pulse window.
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"STATIC", L"Pulse polling regression", WS_POPUP | WS_MINIMIZEBOX,
        -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create isolated window");
    if (!window) return 1;
    check(!ShouldPollChangeTracking(window), "initially hidden window does not poll");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    check(ShouldPollChangeTracking(window), "visible view allows polling");
    ShowWindow(window, SW_HIDE);
    unsigned requests = 0;
    for (unsigned tick = 0; tick < 10000; ++tick)
        if (ShouldPollChangeTracking(window)) ++requests;
    check(requests == 0, "hidden tray window allows no repeated polling");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    check(ShouldPollChangeTracking(window), "restoring from tray resumes polling");
    ShowWindow(window, SW_SHOWMINNOACTIVE);
    check(IsIconic(window) && !ShouldPollChangeTracking(window),
        "minimized window does not poll even when WS_VISIBLE is set");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    check(!IsIconic(window) && ShouldPollChangeTracking(window),
        "restoring from minimized resumes polling");
    DestroyWindow(window);
    check(!ShouldPollChangeTracking(window), "destroyed window does not poll");
    return failures ? 1 : 0;
}
