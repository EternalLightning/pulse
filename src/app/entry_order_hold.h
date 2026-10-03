#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"
#include <string>
#include <vector>

namespace pulse::app {

struct EntryRenameHint {
    std::wstring old_name;
    std::wstring new_name;
};

// Uses physical leaf names, including when the UI shows localized folder labels.
std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                       const std::vector<fs::DirEntry>& fresh,
                                       const std::vector<EntryRenameHint>& renames,
                                       ui::SortColumn column, ui::SortDirection direction,
                                       const std::wstring& parent);

void FollowEntryRenames(const std::vector<EntryRenameHint>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& selected_names, std::wstring& focus_name);
void PruneEntryRenames(std::vector<EntryRenameHint>& renames,
                      const std::vector<fs::DirEntry>& fresh);

} // namespace pulse::app
