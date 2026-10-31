// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/global_search_hotkey.h"
#include <cstdio>

int main() {
    using pulse::GlobalSearchHotkey;
    HWND first = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND second = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!first || !second) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    GlobalSearchHotkey a, b;
    UINT key = VK_F13;
    const UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    for (; key <= VK_F24; ++key) if (a.Update(first, true, modifiers, key)) break;
    if (key > VK_F24) { std::puts("No available isolated test shortcut"); return 2; }
    check(a.Registered(), "enabled shortcut registers");
    check(a.Update(first, true, modifiers, key), "same settings retain registration");
    check(!b.Update(second, true, modifiers, key) && !b.Registered() && b.Error() == ERROR_HOTKEY_ALREADY_REGISTERED,
        "conflicting shortcut reports error without claiming registration");
    check(a.Update(first, false, modifiers, key) && !a.Registered(), "disable releases shortcut");
    check(b.Update(second, true, modifiers, key), "released shortcut can be registered");
    b.Reset();
    check(!b.Update(second, true, 0, key) && b.Error() == ERROR_INVALID_PARAMETER, "unmodified shortcut rejected");
    { GlobalSearchHotkey temporary; check(temporary.Update(first, true, modifiers, key), "lifetime fixture registers"); }
    check(b.Update(second, true, modifiers, key), "destruction unregisters shortcut");
    b.Reset();
    DestroyWindow(first); DestroyWindow(second);
    return failures ? 1 : 0;
}
