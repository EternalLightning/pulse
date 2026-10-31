// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <string_view>
#include "../../third_party/sqlite/sqlite3.h"
namespace pulse::index {
inline int OrdinalCollation(void*, int an, const void* a, int bn, const void* b) {
    const int result = CompareStringOrdinal(static_cast<const wchar_t*>(a), an / 2, static_cast<const wchar_t*>(b), bn / 2, TRUE);
    return result == CSTR_LESS_THAN ? -1 : result == CSTR_GREATER_THAN ? 1 : 0;
}
inline void SqlFilename(sqlite3_context* ctx, int, sqlite3_value** args) {
    const auto* p = static_cast<const wchar_t*>(sqlite3_value_text16(args[0]));
    if (!p) { sqlite3_result_null(ctx); return; }
    std::wstring_view path(p, static_cast<size_t>(sqlite3_value_bytes16(args[0])) / 2);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) path.remove_prefix(slash + 1);
    sqlite3_result_text16(ctx, path.data(), static_cast<int>(path.size() * 2), SQLITE_TRANSIENT);
}
inline void SqlExtension(sqlite3_context* ctx, int, sqlite3_value** args) {
    const auto* p=static_cast<const wchar_t*>(sqlite3_value_text16(args[0]));
    std::wstring_view path=p ? std::wstring_view(p,static_cast<size_t>(sqlite3_value_bytes16(args[0]))/2) : std::wstring_view{};
    const auto slash=path.find_last_of(L"\\/"); if(slash!=std::wstring_view::npos) path.remove_prefix(slash+1);
    const auto dot=path.find_last_of(L'.');
    path=dot==std::wstring_view::npos ? std::wstring_view{} : path.substr(dot+1);
    sqlite3_result_text16(ctx,path.empty() ? L"" : path.data(),static_cast<int>(path.size()*2),SQLITE_TRANSIENT);
}
}
