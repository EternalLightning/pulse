#pragma once
#include "snapshot_patch.h"
#include "../fs/fs_snapshot.h"
#include <map>
#include <memory>
#include <mutex>

namespace pulse {
struct AppState;
namespace app {
struct Tab;

struct NotifyPatchTarget {
    Tab* tab = nullptr; // Compared against live panes before dereferencing.
    fs::SnapshotPtr snapshot;
    uint64_t view_generation = 0;
    uint64_t applied_generation = 0;
};

struct NotifyPatchRequest {
    std::wstring path;
    uint64_t generation = 0;
    std::vector<NotifyPatchTarget> targets;
    ResolvedDirNotifies resolved;
};

struct NotifyPatchMailbox {
    std::mutex mutex;
    std::vector<std::shared_ptr<NotifyPatchRequest>> completed;
};

struct PendingNotifyPatch {
    std::shared_ptr<NotifyPatchRequest> request;
    std::vector<fs::DirNotifyEvent> queued;
};

struct NotifyPatchState {
    uint64_t generation = 0;
    std::map<std::wstring, PendingNotifyPatch> pending;
    std::shared_ptr<NotifyPatchMailbox> mailbox = std::make_shared<NotifyPatchMailbox>();
};
} // namespace app

bool QueueNotifiesToVisible(AppState& s, const std::wstring& path,
                           const std::vector<fs::DirNotifyEvent>& events);
void DrainNotifyPatches(AppState& s);
} // namespace pulse
