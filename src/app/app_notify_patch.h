// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "snapshot_patch.h"
#include "entry_sort.h"
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
    ui::SortColumn column = ui::SortColumn::Name;
    ui::SortDirection direction = ui::SortDirection::Asc;
};

struct NotifyProjection {
    fs::SnapshotPtr source;
    ui::SortColumn column;
    ui::SortDirection direction;
    fs::SnapshotPtr snapshot;
};

struct NotifyPatchRequest {
    std::wstring path;
    uint64_t generation = 0;
    std::vector<NotifyPatchTarget> targets;
    ResolvedDirNotifies resolved;
    FolderSortMode folders = FolderSortMode::FoldersFirst;
    std::vector<NotifyProjection> projections;
};

// Builds immutable projections before the request enters the UI mailbox.
void BuildNotifyProjections(NotifyPatchRequest& request);

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
