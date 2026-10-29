#include "quick_access.h"
#include "quick_access_menu.h"

namespace pulse {
std::vector<std::wstring> QuickAccessTargets(const app::Tab* tab, bool background) {
    if (!tab || IsRecycleTab(tab)) return {};
    if (background) {
        if (tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return {};
        return {tab->current_path};
    }
    if (!tab->snapshot) return {};
    std::vector<std::wstring> paths;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount() ||
            !tab->EntryAt(static_cast<size_t>(index)).is_dir) return {};
        auto path = EntryFullPath(*tab, index);
        if (path.empty() || fs::IsVirtualPath(path)) return {};
        paths.push_back(std::move(path));
    }
    return paths;
}

void AppendQuickAccessCommand(AppState& s, std::vector<ui::FluentMenuItem>& items,
                              const std::vector<std::wstring>& paths) {
    app::AppendQuickAccessMenuCommand(items, s.places, paths);
}

bool HandleQuickAccessCommand(AppState& s, int command,
                              const std::vector<std::wstring>& paths) {
    if (command != app::CmdPinQuickAccess && command != app::CmdUnpinQuickAccess) return false;
    s.places.SetQuickAccessPinned(paths, command == app::CmdPinQuickAccess);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

}
