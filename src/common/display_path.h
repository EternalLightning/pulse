// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <string>
#include <string_view>

namespace pulse::path {

// Presentation only: preserve the original paths used for filesystem operations.
inline std::wstring FriendlyPathText(std::wstring_view text) {
    std::wstring result(text);
    for (const std::wstring_view prefix : {std::wstring_view(L"\\\\?\\"),
                                          std::wstring_view(L"\\??\\")}) {
        size_t pos = 0;
        while ((pos = result.find(prefix, pos)) != std::wstring::npos) {
            const size_t start = pos + prefix.size();
            if (result.size() >= start + 4 &&
                (result[start] == L'U' || result[start] == L'u') &&
                (result[start + 1] == L'N' || result[start + 1] == L'n') &&
                (result[start + 2] == L'C' || result[start + 2] == L'c') &&
                result[start + 3] == L'\\') {
                result.replace(pos, prefix.size() + 4, L"\\\\");
                pos += 2;
            } else if (result.size() >= start + 3 && result[start + 1] == L':' &&
                       result[start + 2] == L'\\') {
                result.erase(pos, prefix.size());
            } else {
                pos = start;
            }
        }
    }
    return result;
}

} // namespace pulse::path
