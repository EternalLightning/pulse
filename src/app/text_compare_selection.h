#pragma once
#include "../common/text_decode.h"
#include <cwctype>
#include <string>

namespace pulse::app {

inline bool IsTextCompareFile(const std::wstring& path, bool directory = false) {
    if (directory || path.empty() || path.starts_with(L"pulse:") ||
        path.starts_with(L"shell:")) return false;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    std::wstring lower = name;
    for (auto& ch : lower) ch = static_cast<wchar_t>(std::towlower(ch));
    const size_t dot = lower.find_last_of(L'.');
    return (dot != std::wstring::npos && text::IsKnownTextExtension(lower.substr(dot))) ||
        lower == L"cmakelists.txt" || lower == L"makefile" || lower == L"dockerfile" ||
        lower == L".gitignore" || lower == L".gitattributes" || lower == L"license";
}

// Owned by the window, so navigation and tab changes never reset the first file.
class TextCompareSelection {
public:
    bool Select(const std::wstring& path) {
        if (!IsTextCompareFile(path)) return false;
        source_ = path;
        return true;
    }
    const std::wstring& source() const { return source_; }
    bool ready() const { return !source_.empty(); }
private:
    std::wstring source_;
};

} // namespace pulse::app
