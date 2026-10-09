#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_renderer.h"
#include <vector>

namespace pulse::app {

enum class NotifyPatch { Applied, NeedFullEnum };

struct ResolvedDirNotifies {
    NotifyPatch status = NotifyPatch::NeedFullEnum;
    std::vector<fs::DirNotifyEvent> events;
    // One fact per event; removed events have an empty entry.
    std::vector<fs::DirEntry> entries;
};

// May query the filesystem. Call on a worker before applying to UI snapshots.
ResolvedDirNotifies ResolveDirNotifies(const std::wstring& folder,
                                     const std::vector<fs::DirNotifyEvent>& events);
// Applies already resolved facts atomically, without filesystem access.
NotifyPatch ApplyResolvedDirNotifies(std::vector<fs::DirEntry>& entries,
                                    const ResolvedDirNotifies& resolved);

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
