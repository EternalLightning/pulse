// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "path_utils.h"
#include <initializer_list>
#include <string_view>

namespace pulse::path {

inline bool IsValidFileName(std::wstring_view name) {
    // Component length is enforced by the destination filesystem, not a fixed UI limit.
    if (name.empty() || name.back() == L'.' || name.back() == L' ') return false;
    for (const auto c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos) return false;
    auto stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
    for (const std::wstring_view reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (stem.size() == reserved.size() && EqualInsensitive(stem, reserved))
            return false;
    return !(stem.size() == 4 && ((stem[3] >= L'1' && stem[3] <= L'9') ||
        stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3') &&
        (EqualInsensitive(stem.substr(0, 3), L"COM") || EqualInsensitive(stem.substr(0, 3), L"LPT")));
}

} // namespace pulse::path
