// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#pragma once

#include <windows.h>
#include <cwchar>

namespace pulse::dialog_bridge {

inline void NativeFallbackName(wchar_t (&name)[64], DWORD process) noexcept {
    swprintf_s(name, L"Local\\Pulse.DialogBridge.Native.%lu", process);
}

class NativeFallbackScope {
public:
    NativeFallbackScope() noexcept {
        wchar_t name[64]{};
        NativeFallbackName(name, GetCurrentProcessId());
        marker_ = CreateEventW(nullptr, TRUE, TRUE, name);
    }
    ~NativeFallbackScope() { if (marker_) CloseHandle(marker_); }
    NativeFallbackScope(const NativeFallbackScope&) = delete;
    NativeFallbackScope& operator=(const NativeFallbackScope&) = delete;
private:
    HANDLE marker_ = nullptr;
};

// A marker can only opt a caller out of window takeover.
inline bool HasNativeFallback(DWORD process) noexcept {
    wchar_t name[64]{};
    NativeFallbackName(name, process);
    const HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!marker) return false;
    CloseHandle(marker);
    return true;
}

inline void NativeModeName(wchar_t (&name)[96], DWORD process, DWORD thread, HWND owner, bool open) noexcept {
    swprintf_s(name, L"Local\\Pulse.DialogBridge.NativeMode.%lu.%lu.%llx.%d", process, thread,
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(owner)), open ? 1 : 0);
}

class NativeModeScope {
public:
    NativeModeScope(HWND owner, bool open) noexcept {
        wchar_t name[96]{};
        NativeModeName(name, GetCurrentProcessId(), GetCurrentThreadId(), owner, open);
        marker_ = CreateEventW(nullptr, TRUE, TRUE, name);
    }
    ~NativeModeScope() { if (marker_) CloseHandle(marker_); }
    NativeModeScope(const NativeModeScope&) = delete;
    NativeModeScope& operator=(const NativeModeScope&) = delete;
private:
    HANDLE marker_ = nullptr;
};

inline bool HasNativeOpenMode(DWORD process, DWORD thread, HWND owner) noexcept {
    const auto has_mode = [&](bool open) {
        wchar_t name[96]{};
        NativeModeName(name, process, thread, owner, open);
        const HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!marker) return false;
        CloseHandle(marker);
        return true;
    };
    // An overlapping native show with the same owner makes the mode ambiguous.
    return has_mode(true) && !has_mode(false);
}

} // namespace pulse::dialog_bridge
