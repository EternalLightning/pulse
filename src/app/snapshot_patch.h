#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include <vector>

namespace pulse::app {

// Snapshot/notification identity uses exact physical leaf names from enumeration
// and watcher events. Case folding belongs to display sorting, never row lookup.
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

} // namespace pulse::app
