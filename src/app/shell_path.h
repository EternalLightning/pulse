#pragma once
#include "../common/path_utils.h"
#include <algorithm>
#include <cwctype>

namespace pulse::app {
// Shell boundaries use ordinary paths. Do not run virtual/empty paths through
// GetFullPathName/NormalizePath: empty is This PC, not the current directory.
inline std::wstring ShellPathText(std::wstring_view value) {
    if (value.size() >= 8 && path::EqualInsensitive(value.substr(0, 8), L"\\\\?\\UNC\\"))
        return L"\\\\" + std::wstring(value.substr(8));
    return path::StripExtendedPathPrefix(value);
}
inline std::wstring ShellPathIdentity(std::wstring_view value) {
    std::wstring result = ShellPathText(value);
    const bool local = result.size() >= 3 && iswalpha(result[0]) && result[1] == L':' &&
        (result[2] == L'\\' || result[2] == L'/');
    const bool unc = result.size() >= 2 && result[0] == L'\\' && result[1] == L'\\';
    if (!local && !unc) return result;
    std::replace(result.begin(), result.end(), L'/', L'\\');
    const size_t minimum = local ? 3 : 2;
    while (result.size() > minimum && result.back() == L'\\') result.pop_back();
    return result;
}
inline bool SameShellPath(std::wstring_view left, std::wstring_view right) {
    const auto a = ShellPathIdentity(left);
    const auto b = ShellPathIdentity(right);
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return path::EqualInsensitive(a, b);
}
} // namespace pulse::app
