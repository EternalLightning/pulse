#pragma once

#include <windows.h>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

using AboutRow = std::pair<std::wstring, std::wstring>;  // label, value

inline constexpr const wchar_t* kPulseHomepage = L"https://github.com/jimmgreen/pulse";

// Label/value rows for Settings > About (version, build, Windows, location, ...).
std::vector<AboutRow> BuildAboutRows(bool index_service, bool index_installed, float scale);

// Plain-text block suitable for bug reports.
std::wstring AboutRowsText(const std::vector<AboutRow>& rows);

bool CopyTextToClipboard(HWND owner, const std::wstring& text);

} // namespace pulse::app
