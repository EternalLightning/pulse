#pragma once
#include "app_model.h"
#include "shell_path.h"
#include <shlobj.h>

namespace pulse::app {
inline std::wstring ShellEntryPath(const Tab& tab, int index) {
    const auto entry = tab.EntryAt(static_cast<size_t>(index));
    if (entry.change_record_only) return {};
    if (!entry.full_path.empty()) return entry.full_path;
    if (tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) return {};
    return tab.current_path + (tab.current_path.ends_with(L"\\") ? L"" : L"\\") + entry.name;
}
// Resolve *all* items before changing selection; hidden/filtered items must not
// acknowledge a takeover and cause Explorer to close without a visible result.
inline bool ApplyShellSelection(Tab& tab, const PlacesCatalog& places,
                                const std::vector<std::wstring>& paths, unsigned flags,
                                std::vector<int>& rows) {
    rows.clear();
    if (tab.loading || tab.pending_generation || !tab.snapshot || tab.net_readonly ||
        !tab.banner_message.empty() || !SameShellPath(tab.snapshot_path, tab.current_path)) return false;
    std::vector<int> visible;
    CollectFilterMatches(tab, &places, visible);
    for (const auto& wanted : paths) {
        const auto found = std::find_if(visible.begin(), visible.end(), [&](int row) {
            return tab.EntryVisible(row) && SameShellPath(ShellEntryPath(tab, row), wanted);
        });
        if (wanted.empty() || found == visible.end()) { rows.clear(); return false; }
        rows.push_back(*found);
    }
    if (flags & SVSI_DESELECTOTHERS) tab.ClearSelection();
    const bool select = (flags & SVSI_SELECT) != 0;
    const bool deselect = flags == SVSI_DESELECT;
    for (int row : rows) {
        // SVSI_DESELECT is zero. Focus/ensure/edit-only requests do not change
        // selection, and EDIT never starts a rename on another caller's behalf.
        if ((select && !tab.IsSelected(row)) || (deselect && tab.IsSelected(row))) tab.ToggleSelect(row);
        if (select || (flags & (SVSI_FOCUSED | SVSI_EDIT))) {
            tab.selected_index = row;
            tab.selection_anchor = row;
        }
    }
    return std::all_of(rows.begin(), rows.end(), [&](int row) {
        return tab.EntryVisible(row) && (!select || tab.IsSelected(row)) &&
            (!deselect || !tab.IsSelected(row)) && std::find(visible.begin(), visible.end(), row) != visible.end();
    });
}
inline bool ShellSelectionIsStale(const Tab& tab, std::wstring_view folder,
                                  uint64_t generation, uint64_t view_generation) {
    return !SameShellPath(tab.current_path, folder) || tab.view_generation != view_generation ||
        (tab.pending_generation ? tab.pending_generation != generation : tab.applied_generation != generation);
}
} // namespace pulse::app
