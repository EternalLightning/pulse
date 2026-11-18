// SPDX-License-Identifier: Apache-2.0
// Adapted from the Pulse upstream dialog auto-registration module.

#pragma once

#include <windows.h>
#include <string>

namespace pulse::dialog_bridge {

inline constexpr wchar_t DllName[] = L"Pulse.DialogBridge.dll";
inline constexpr wchar_t ConfigKey[] = L"Software\\Pulse\\FileDialogBridge";
inline constexpr REGSAM RegistryView = KEY_WOW64_64KEY;

struct Status {
    bool enabled = false;
    bool conflict = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring dll_path;
};

DWORD Enable(const std::wstring& absolute_dll);
DWORD DisableFor(const std::wstring& absolute_dll);
Status Inspect();
bool IsEnabledFor(const std::wstring& absolute_dll) noexcept;

} // namespace pulse::dialog_bridge
