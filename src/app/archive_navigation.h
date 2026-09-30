#pragma once
#include <string>
#include <windows.h>
namespace pulse {
struct AppState;
namespace app { struct Tab; }
constexpr UINT WM_ARCHIVE_RESULT = WM_APP + 66;
bool IsArchiveView(const std::wstring& path);
std::wstring ArchiveViewPath(const std::wstring& file, const std::wstring& prefix = {});
void LoadArchiveView(AppState& s, app::Tab& tab);
bool OpenArchiveSelection(AppState& s);
void ExtractArchiveSelection(AppState& s, bool all);
void HandleArchiveResult(AppState& s, LPARAM payload);
void DiscardArchiveResult(LPARAM payload);
std::wstring ArchiveParent(const std::wstring& path);
void CleanupArchiveFiles();
}
