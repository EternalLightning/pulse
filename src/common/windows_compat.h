// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <shellscalingapi.h>

namespace pulse::compat {
inline FARPROC UserApi(const char* name) noexcept {
    return GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
}

inline UINT WindowDpi(HWND hwnd) noexcept {
    using Fn = UINT(WINAPI*)(HWND);
    static const auto fn = reinterpret_cast<Fn>(UserApi("GetDpiForWindow"));
    if (fn) {
        const UINT dpi = fn(hwnd);
        if (dpi) return dpi;
    }
    UINT x = 96, y = 96;
    if (SUCCEEDED(GetDpiForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                                 MDT_EFFECTIVE_DPI, &x, &y)) && x) return x;
    HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

inline int SystemMetricsForDpi(int metric, UINT dpi) noexcept {
    using Fn = int(WINAPI*)(int, UINT);
    static const auto fn = reinterpret_cast<Fn>(UserApi("GetSystemMetricsForDpi"));
    if (fn) return fn(metric, dpi);
    HDC dc = GetDC(nullptr);
    const int system_dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return MulDiv(GetSystemMetrics(metric), static_cast<int>(dpi), system_dpi > 0 ? system_dpi : 96);
}

inline void EnableDpiAwareness() noexcept {
    using Fn = BOOL(WINAPI*)(HANDLE);
    static const auto fn = reinterpret_cast<Fn>(UserApi("SetProcessDpiAwarenessContext"));
    if (fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
}
}

#pragma comment(lib, "shcore.lib")
