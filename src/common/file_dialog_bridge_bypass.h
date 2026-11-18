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

} // namespace pulse::dialog_bridge
