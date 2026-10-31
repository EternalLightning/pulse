// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {

struct ArchiveExtractOptions {
    std::wstring destination;
    std::wstring password;
    bool create_folder = true;
};

bool ShowArchiveExtractDialog(HWND owner, bool dark, COLORREF accent,
                              const std::wstring& archive_path, bool all,
                              ArchiveExtractOptions& options);
bool ShowArchivePasswordDialog(HWND owner, bool dark, COLORREF accent,
                               std::wstring& password);

} // namespace pulse::app
