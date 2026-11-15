// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_types.h"
#include <cstring>
#include <windows.h>
#include <shlwapi.h>
#include <string_view>

namespace pulse::index {
inline int CompareSearchNames(std::wstring_view a, std::wstring_view b) {
    wchar_t wa[260], wb[260];
    if (a.size() < 259 && b.size() < 259) {
        if (!a.empty()) memcpy(wa, a.data(), a.size() * sizeof(wchar_t));
        wa[a.size()] = 0;
        if (!b.empty()) memcpy(wb, b.data(), b.size() * sizeof(wchar_t));
        wb[b.size()] = 0;
        return StrCmpLogicalW(wa, wb);
    }
    return StrCmpLogicalW(std::wstring(a).c_str(), std::wstring(b).c_str());
}

// Equal keys retain provider order: local node IDs (reversed for descending sorts), network records.
inline int CompareSearchOrder(std::wstring_view a_name, bool a_dir, uint64_t a_size,
                              uint64_t a_mtime, std::wstring_view b_name, bool b_dir,
                              uint64_t b_size, uint64_t b_mtime, ResultSort sort, bool desc) {
    if ((sort == ResultSort::Size || sort == ResultSort::Mtime) && a_dir != b_dir)
        return a_dir ? -1 : 1;
    const uint64_t a_value = sort == ResultSort::Size ? a_size : a_mtime;
    const uint64_t b_value = sort == ResultSort::Size ? b_size : b_mtime;
    int comparison = sort != ResultSort::Name && a_value != b_value
        ? (a_value < b_value ? -1 : 1) : CompareSearchNames(a_name, b_name);
    return desc ? -comparison : comparison;
}
} // namespace pulse::index
