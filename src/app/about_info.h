// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

using AboutRow = std::pair<std::wstring, std::wstring>;  // label, value

inline constexpr const wchar_t* kPulseHomepage = L"https://github.com/EternalLightning/pulse";
inline constexpr const wchar_t* kPulseUpstream = L"https://github.com/jimmgreen/pulse";

// Label/value rows for Settings > About (version, build, Windows, location, ...).
std::vector<AboutRow> BuildAboutRows(bool index_service, bool index_installed, float scale);

// Plain-text block suitable for bug reports.
std::wstring AboutRowsText(const std::vector<AboutRow>& rows);

bool CopyTextToClipboard(HWND owner, const std::wstring& text);

} // namespace pulse::app
