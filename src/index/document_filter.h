// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::index {
HRESULT ExtractOfficeFilter(const std::wstring& path, std::wstring& output,
                           std::wstring_view stop_needle = {}, bool case_sensitive = false);
}
