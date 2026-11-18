// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace pulse::portable_device {

inline std::wstring ShellItemName(IShellItem* item) {
    Microsoft::WRL::ComPtr<IShellItem2> item2;
    PWSTR raw = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2)))) item2->GetString(PKEY_FileName, &raw);
    std::wstring name = raw ? raw : L"";
    CoTaskMemFree(raw);
    if (name.empty()) {
        raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) && raw) name = raw;
        CoTaskMemFree(raw);
    }
    return name;
}

inline bool IsDevice(IShellItem* item, std::wstring& parsing) {
    SFGAOF attrs = 0;
    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attrs)) ||
        (attrs & SFGAO_FILESYSTEM) || !(attrs & SFGAO_FOLDER)) return false;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw))) return false;
    parsing = raw ? raw : L"";
    CoTaskMemFree(raw);
    std::wstring lower = parsing;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return lower.find(L"\\\\?\\usb#") != std::wstring::npos || lower.find(L"\\\\?\\swd#wpdbusenum") != std::wstring::npos;
}

} // namespace pulse::portable_device
