#pragma once
#include "../fs/fs_enum.h"
#include <string>
#include <vector>

namespace pulse::app {

struct EntryRenameHint {
    std::wstring old_name;
    std::wstring new_name;
};

// Rename and selection identity use exact physical leaf names.
void FollowEntryRenames(const std::vector<EntryRenameHint>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& selected_names, std::wstring& focus_name);
void PruneEntryRenames(std::vector<EntryRenameHint>& renames,
                      const std::vector<fs::DirEntry>& fresh);

} // namespace pulse::app
