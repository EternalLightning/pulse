#include "app_internal.h"
#include "app_column_view.h"
#include "entry_order_hold.h"
#include <algorithm>

namespace pulse {
namespace {

bool PublishNotifyPatch(AppState& s, const app::NotifyPatchRequest& request, bool prune_renames = true) {
    const auto& path = request.path;
    const auto& events = request.resolved.events;
    if (request.resolved.status != app::NotifyPatch::Applied || PathHasPendingRefresh(s, path))
        return false;
    std::vector<app::Tab*> tabs;
    bool stale = false;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path) return;
        const auto found = std::find_if(request.targets.begin(), request.targets.end(),
            [&](const auto& target) { return target.tab == tab; });
        if (found == request.targets.end() || found->snapshot != tab->snapshot ||
            request.generation != tab->notify_patch_generation ||
            found->view_generation != tab->view_generation ||
            found->applied_generation != tab->applied_generation || tab->loading ||
            s.explicit_entry_refreshes.contains(tab)) stale = true;
        else tabs.push_back(tab);
    });
    if (stale || tabs.empty()) return false;
    std::vector<std::pair<fs::SnapshotPtr, fs::SnapshotPtr>> patches;
    for (auto* tab : tabs) {
        const auto found = std::find_if(patches.begin(), patches.end(),
            [&](const auto& patch) { return patch.first == tab->snapshot; });
        if (found != patches.end()) continue;
        auto copy = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        if (app::ApplyResolvedDirNotifies(*copy, request.resolved) != app::NotifyPatch::Applied)
            return false;
        patches.emplace_back(tab->snapshot, std::move(copy));
    }
    s.store.Put(path, patches.front().second);
    InvalidateColumnStripPath(s, path, patches.front().second);
    for (auto* tab : tabs) {
        const auto found = std::find_if(patches.begin(), patches.end(), [&](const auto& patch) {
            return patch.first == tab->snapshot;
        });
        const auto& patched = found->second;
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
                if (_wcsicmp(name.c_str(), event.old_name.c_str()) == 0) name = event.name;
            }
            if (_wcsicmp(focus.c_str(), event.old_name.c_str()) == 0) focus = event.name;
        }
        tab->order_held = true;
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
    bool unavailable = false;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || tab->current_path != path) return;
        if (!tab->snapshot || tab->loading || s.explicit_entry_refreshes.contains(tab)) {
            unavailable = true;
            return;
        }
        tab->notify_patch_generation = request->generation;
        request->targets.push_back({tab, tab->snapshot, tab->view_generation, tab->applied_generation});
    });
    if (unavailable || request->targets.empty()) return false;
    // Pure removals need no filesystem query and can publish in this tick.
    if (std::all_of(events.begin(), events.end(), [](const auto& event) {
            return event.action == FILE_ACTION_REMOVED;
        })) {
        request->resolved.events = events;
        request->resolved.entries.resize(events.size());
        request->resolved.status = app::NotifyPatch::Applied;
        return PublishNotifyPatch(s, *request);
    }
    state.pending.emplace(path, app::PendingNotifyPatch{request, {}});
    const auto mailbox = state.mailbox;
    if (!s.worker.EnqueueIo([request, events] {
            request->resolved = app::ResolveDirNotifies(request->path, events);
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
