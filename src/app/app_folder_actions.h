#pragma once
#include "app_model.h"
#include "../fs/fs_enum.h"
#include "../common/path_utils.h"

namespace pulse {
struct AppState;
namespace app {
inline bool FolderActionSelection(const app::Tab* tab, bool dissolve) {
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) ||
        tab->net_readonly || tab->content_results ||
        !tab->snapshot) return false;
    auto plain = path::StripExtendedPathPrefix(tab->current_path);
    while (plain.ends_with(L"\\")) plain.pop_back();
    if (plain.starts_with(L"\\\\") && plain.find(L'\\', 2) == std::wstring::npos) return false;
    const auto selected = tab->SelectedIndices();
    if (selected.size() < (dissolve ? 1u : 2u)) return false;
    for (const int index : selected) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount()) return false;
        const auto& entry = tab->EntryAt(static_cast<size_t>(index));
        if (entry.change_record_only || !entry.recycle_path.empty() ||
            (dissolve && (!entry.is_dir || entry.is_reparse))) return false;
        const auto full = entry.full_path.empty()
            ? tab->current_path + (tab->current_path.ends_with(L"\\") ? L"" : L"\\") + entry.name
            : entry.full_path;
        if (fs::IsVirtualPath(full) || path::IsDriveRootPath(full) ||
            !path::EqualInsensitive(path::StripExtendedPathPrefix(fs::ParentPath(full)),
                path::StripExtendedPathPrefix(tab->current_path))) return false;
    }
    return true;
}
}

inline bool CanGroupSelection(const app::Tab* tab) { return app::FolderActionSelection(tab, false); }
inline bool CanDissolveSelection(const app::Tab* tab) { return app::FolderActionSelection(tab, true); }

void GroupSelectionIntoFolder(AppState& s);
void DissolveSelectedFolders(AppState& s);
} // namespace pulse
