// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <cwchar>

namespace pulse::bench {

inline bool IsOwnedThreadWindow(HWND window, HWND owner, const wchar_t* class_name) {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    wchar_t name[256]{};
    return owner && IsWindow(owner) && IsWindow(window) && IsWindowVisible(window) &&
        thread == GetCurrentThreadId() && process == GetCurrentProcessId() &&
        GetWindow(window, GW_OWNER) == owner &&
        GetClassNameW(window, name, ARRAYSIZE(name)) && !std::wcscmp(name, class_name);
}

inline HWND FindOwnedThreadWindow(HWND owner, const wchar_t* class_name) {
    struct Search { HWND owner; const wchar_t* class_name; HWND window = nullptr; } search{owner, class_name};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM parameter) -> BOOL {
        auto& current = *reinterpret_cast<Search*>(parameter);
        if (!IsOwnedThreadWindow(window, current.owner, current.class_name)) return TRUE;
        current.window = window;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

} // namespace pulse::bench
