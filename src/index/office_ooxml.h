// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <string>
#include <string_view>
#include "document_metrics.h"

namespace pulse::index {

// Caller initializes COM. Failure always clears output; no package data is written to disk.
HRESULT ExtractOfficeOpenXml(const std::wstring& path, std::wstring& output,
                            std::wstring_view stop_needle = {}, bool case_sensitive = false,
                            DocumentReadMetrics* metrics = nullptr);

}
