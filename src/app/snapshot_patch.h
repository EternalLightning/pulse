#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_renderer.h"
#include <vector>

namespace pulse::app {

enum class NotifyPatch { Applied, NeedFullEnum };

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out);

NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir,
                           bool keep_order = false);

// Same result as calling ApplyDirNotify for each event in order, in
// O(entries + events log events) instead of O(entries * events). With
// keep_order, replace existing rows in place and append newly added rows.
NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir,
                                bool keep_order = false);

} // namespace pulse::app
