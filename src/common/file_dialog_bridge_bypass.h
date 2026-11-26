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

enum class NativeMode { Unknown, Open, Save };

inline void NativeModeName(wchar_t (&name)[96], DWORD process, DWORD thread, HWND owner, NativeMode mode) noexcept {
    swprintf_s(name, L"Local\\Pulse.DialogBridge.NativeMode.%lu.%lu.%llx.%d", process, thread,
        static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(owner)), static_cast<int>(mode));
}

class NativeModeScope {
public:
    NativeModeScope(HWND owner, NativeMode mode) noexcept {
        wchar_t name[96]{};
        NativeModeName(name, GetCurrentProcessId(), GetCurrentThreadId(), owner, mode);
        marker_ = CreateEventW(nullptr, TRUE, TRUE, name);
    }
    ~NativeModeScope() { if (marker_) CloseHandle(marker_); }
    NativeModeScope(const NativeModeScope&) = delete;
    NativeModeScope& operator=(const NativeModeScope&) = delete;
private:
    HANDLE marker_ = nullptr;
};

inline NativeMode QueryNativeMode(DWORD process, DWORD thread, HWND owner) noexcept {
    const auto has_mode = [&](NativeMode mode) {
        wchar_t name[96]{};
        NativeModeName(name, process, thread, owner, mode);
        const HANDLE marker = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (!marker) return false;
        CloseHandle(marker);
        return true;
    };
    // An overlapping native show with the same owner makes the mode ambiguous.
    const bool open = has_mode(NativeMode::Open), save = has_mode(NativeMode::Save);
    if (open == save || has_mode(NativeMode::Unknown)) return NativeMode::Unknown;
    return open ? NativeMode::Open : NativeMode::Save;
}

} // namespace pulse::dialog_bridge
