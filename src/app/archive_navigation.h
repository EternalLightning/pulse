// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string>
#include <windows.h>
#include "../ops/archive.h"
namespace pulse {
struct AppState;
namespace app { struct Tab; }
constexpr UINT WM_ARCHIVE_RESULT = WM_APP + 66;
bool IsArchiveView(const std::wstring& path);
std::wstring ArchiveViewPath(const std::wstring& file, const std::wstring& prefix = {});
void LoadArchiveView(AppState& s, app::Tab& tab, bool refresh = false);
bool OpenArchiveSelection(AppState& s);
void ExtractArchiveSelection(AppState& s, bool all);
void UpdateArchiveOpenPassword(app::Tab& tab, const std::wstring& source_view,
    uint64_t source_generation, const std::wstring& file, const std::wstring& password,
    const ops::ArchiveOperationResult& result);
void HandleArchiveResult(AppState& s, LPARAM payload);
void DiscardArchiveResult(LPARAM payload);
std::wstring ArchiveParent(const std::wstring& path);
void CleanupArchiveFiles();
}
