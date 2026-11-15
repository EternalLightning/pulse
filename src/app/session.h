// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// session.h — Window preference persistence and legacy migration.
#pragma once
#include "app_model.h"
#include <array>
#include <string>
#include <windows.h>

namespace pulse::app {

inline bool WindowPlacementRestoresMaximized(const WINDOWPLACEMENT& placement) {
    return placement.showCmd == SW_SHOWMAXIMIZED ||
        ((placement.flags & WPF_RESTORETOMAXIMIZED) != 0 &&
         (placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE ||
          placement.showCmd == SW_SHOWMINNOACTIVE));
}

struct WindowSessionSnapshot {
    RECT window_rect = {};
    bool maximized = false;
    bool dark = false; // legacy serialization/migration, projected from window appearance
    int sidebar_collapsed = 0; // bitmask indexed by SidebarSectionId
    int sidebar_hidden = 0;    // same ids; hidden sections are not drawn at all
    std::vector<int> sidebar_order; // display order, empty = DefaultSidebarOrder()
    int quick_access_hidden = 0;    // bitmask over BuiltinQuickAccess links
    bool starred_expanded = true;
    bool details_panel = false;  // right details panel visible
    int details_panel_width = 340;
    bool details_preview_only = false;
    bool details_preview = true; // preview pane enabled (session remembers it)
    std::array<float, 3> details_column_dividers{}; // version 3 migration only
};

std::wstring GetPulseDataDir();
// preferences.json retains appearance. Legacy session.json is read only when
// no preferences file exists; staging tray and undo history stay in memory.
// Startup never restores navigation, staging tray, or undo history.
bool SaveSessionPreferences(const WindowSessionSnapshot& snap);
std::wstring SessionPreferencesToJson(const WindowSessionSnapshot& snap);
// Automatic saves must retain this permission separately from recaptured snapshots.
bool LoadSessionPreferences(WindowSessionSnapshot& snap, bool* saves_enabled = nullptr);

} // namespace pulse::app
