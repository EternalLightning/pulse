// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "app_column_view.h"
#include "entry_order_hold.h"
#include "directory_projection.h"
#include <algorithm>
#include <iterator>

namespace pulse {
namespace app {
void BuildNotifyProjections(NotifyPatchRequest& request) {
    request.projections.clear();
    if (request.resolved.status != NotifyPatch::Applied) return;
    std::vector<std::pair<fs::SnapshotPtr, fs::SnapshotPtr>> facts;
    std::vector<NotifyProjection> projections;
    for (const auto& target : request.targets) {
        const auto existing = std::find_if(projections.begin(), projections.end(),
            [&](const auto& projection) { return projection.source == target.snapshot &&
                projection.column == target.column && projection.direction == target.direction; });
        if (existing != projections.end()) continue;
        auto source = std::find_if(facts.begin(), facts.end(),
            [&](const auto& fact) { return fact.first == target.snapshot; });
        if (source == facts.end()) {
            auto patched = std::make_shared<std::vector<fs::DirEntry>>(*target.snapshot);
            if (ApplyResolvedDirNotifies(*patched, request.resolved) != NotifyPatch::Applied) return;
            facts.emplace_back(target.snapshot, std::move(patched));
            source = std::prev(facts.end());
        }
        auto projected = ProjectDirectorySnapshot(source->second, request.path,
            target.column, target.direction, request.folders);
        projections.push_back({target.snapshot, target.column, target.direction, std::move(projected)});
    }
    request.projections = std::move(projections);
}
} // namespace app
namespace {

bool PublishNotifyPatch(AppState& s, const app::NotifyPatchRequest& request, bool prune_renames = true) {
    const auto& path = request.path;
    const auto& events = request.resolved.events;
    if (request.resolved.status != app::NotifyPatch::Applied ||
        request.folders != app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode) || PathHasPendingRefresh(s, path))
        return false;
    std::vector<std::pair<app::Tab*, fs::SnapshotPtr>> tabs;
    bool stale = false;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path) return;
        const auto found = std::find_if(request.targets.begin(), request.targets.end(),
            [&](const auto& target) { return target.tab == tab; });
        if (found == request.targets.end() || found->snapshot != tab->snapshot ||
            request.generation != tab->notify_patch_generation ||
            found->view_generation != tab->view_generation ||
            found->applied_generation != tab->applied_generation ||
            found->column != tab->sort_column || found->direction != tab->sort_direction || tab->loading ||
            s.explicit_entry_refreshes.contains(tab)) stale = true;
        else {
            const auto projection = std::find_if(request.projections.begin(), request.projections.end(),
                [&](const auto& item) { return item.source == tab->snapshot &&
                    item.column == tab->sort_column && item.direction == tab->sort_direction; });
            if (projection == request.projections.end() || !projection->snapshot) stale = true;
            else tabs.emplace_back(tab, projection->snapshot);
        }
    });
    if (stale || tabs.empty()) return false;
    s.store.Put(path, tabs.front().second);
    InvalidateColumnStripPath(s, path, tabs.front().second);
    for (const auto& [tab, patched] : tabs) {
        std::vector<std::wstring> names;
        std::wstring focus;
        if (tab->SelectedCount() > 0) {
            for (int index : tab->SelectedIndices()) {
                if (index >= 0 && index < static_cast<int>(tab->EntryCount()))
                    names.push_back(tab->EntryAt(static_cast<size_t>(index)).name);
            }
            if (tab->selected_index >= 0 &&
                tab->selected_index < static_cast<int>(tab->EntryCount()))
                focus = tab->EntryAt(static_cast<size_t>(tab->selected_index)).name;
        }
        for (const auto& event : events) {
            if (event.action != FILE_ACTION_RENAMED_NEW_NAME || event.old_name.empty()) continue;
            for (auto& name : names) {
                if (name == event.old_name) name = event.name;
            }
            if (focus == event.old_name) focus = event.name;
        }
        if (prune_renames) app::PruneEntryRenames(tab->held_renames, *patched);
        tab->SetSnapshot(patched);
        if (!names.empty()) tab->RemapSelection(names, focus);
        else if (tab->snapshot && tab->EntryCount() != 0 && tab->selected_index < 0)
            tab->SelectOnly(0);
        RevealPendingCreatedItem(s, *tab);
    }
    SyncQuickPreview(s);
    return true;
}

void FallbackNotifyPatch(AppState& s, const std::wstring& path) {
    s.notify_patches.pending.erase(path);
    DropSizePatches(s, path);
    s.store.MarkDirty(path);
    InvalidateColumnStripPath(s, path);
    RefreshPath(s, path);
}

} // namespace

bool QueueNotifiesToVisible(AppState& s, const std::wstring& path,
                           const std::vector<fs::DirNotifyEvent>& events) {
    if (events.empty()) return true;
    constexpr size_t kMaxIncrementalEvents = 512;
    if (path.empty() || fs::IsVirtualPath(path) || events.size() > kMaxIncrementalEvents ||
        PathHasPendingRefresh(s, path)) return false;
    for (const auto& event : events) {
        if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos ||
            event.old_name.find_first_of(L"\\/") != std::wstring::npos ||
            (event.action != FILE_ACTION_ADDED && event.action != FILE_ACTION_REMOVED &&
             event.action != FILE_ACTION_MODIFIED && event.action != FILE_ACTION_RENAMED_NEW_NAME))
            return false;
    }
    auto& state = s.notify_patches;
    if (auto found = state.pending.find(path); found != state.pending.end()) {
        if (found->second.queued.size() + events.size() > kMaxIncrementalEvents) return false;
        found->second.queued.insert(found->second.queued.end(), events.begin(), events.end());
        return true;
    }
    auto request = std::make_shared<app::NotifyPatchRequest>();
    request->path = path;
    request->generation = ++state.generation;
    request->folders = app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode);
    bool unavailable = false;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path) return;
        if (!tab->snapshot || tab->loading || s.explicit_entry_refreshes.contains(tab)) {
            unavailable = true;
            return;
        }
        tab->notify_patch_generation = request->generation;
        request->targets.push_back({tab, tab->snapshot, tab->view_generation, tab->applied_generation,
                                   tab->sort_column, tab->sort_direction});
    });
    if (unavailable || request->targets.empty()) return false;
    state.pending.emplace(path, app::PendingNotifyPatch{request, {}});
    const auto mailbox = state.mailbox;
    if (!s.worker.EnqueueIo([request, events] {
            request->resolved = app::ResolveDirNotifies(request->path, events);
            app::BuildNotifyProjections(*request);
        }, [request, mailbox] {
            std::lock_guard lock(mailbox->mutex);
            mailbox->completed.push_back(request);
        })) {
        state.pending.erase(path);
        return false;
    }
    return true;
}

void DrainNotifyPatches(AppState& s) {
    std::vector<std::shared_ptr<app::NotifyPatchRequest>> completed;
    {
        std::lock_guard lock(s.notify_patches.mailbox->mutex);
        completed.swap(s.notify_patches.mailbox->completed);
    }
    for (const auto& request : completed) {
        auto found = s.notify_patches.pending.find(request->path);
        if (found == s.notify_patches.pending.end() ||
            found->second.request->generation != request->generation) continue;
        auto queued = std::move(found->second.queued);
        s.notify_patches.pending.erase(found);
        if (!PublishNotifyPatch(s, *request, queued.empty()) ||
            (!queued.empty() && !QueueNotifiesToVisible(s, request->path, queued)))
            FallbackNotifyPatch(s, request->path);
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

} // namespace pulse
