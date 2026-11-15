// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <objbase.h>
#include <cwctype>
#include <string>

namespace pulse::model::detail {

inline std::wstring NewTagId(HRESULT (WINAPI* create_guid)(GUID*) = CoCreateGuid) {
    GUID guid{};
    if (FAILED(create_guid(&guid))) return {};
    wchar_t text[40]{};
    if (!StringFromGUID2(guid, text, ARRAYSIZE(text))) return {};
    std::wstring id(text + 1, 36);
    for (auto& c : id) c = static_cast<wchar_t>(std::towlower(c));
    return id;
}

} // namespace pulse::model::detail
