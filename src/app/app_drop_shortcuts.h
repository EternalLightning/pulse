#pragma once
#include <windows.h>
#include <oleidl.h>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app {

// OLE does not include Alt in grfKeyState; the target supplies this bit.
inline constexpr DWORD kDropAltKey = 0x20;
inline bool ShortcutDropRequested(DWORD keys) {
    return (keys & kDropAltKey) != 0 ||
        (keys & (MK_CONTROL | MK_SHIFT)) == (MK_CONTROL | MK_SHIFT);
}

bool IsShortcutFileSystemPath(const std::wstring& path);
bool CanCreateDropShortcuts(const std::vector<std::wstring>& sources, const std::wstring& destination);

struct ShortcutDropResult {
    std::wstring destination;
    size_t created = 0;
    size_t failed = 0;
    std::wstring failed_source;
    HRESULT error = S_OK;
};

// Blocking filesystem and Shell work. Call only from a background worker.
ShortcutDropResult CreateDropShortcuts(const std::vector<std::wstring>& sources,
    const std::wstring& destination, const std::wstring& name_format);

} // namespace app

bool QueueDropShortcuts(AppState& state, const std::vector<std::wstring>& sources,
    const std::wstring& destination);
void HandleDropShortcutsResult(AppState& state, WPARAM identity);
} // namespace pulse
