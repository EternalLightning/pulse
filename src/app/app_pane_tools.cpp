// SPDX-License-Identifier: Apache-2.0
#include "app_pane_tools.h"
#include "app_internal.h"
#include "command_catalog.h"
#include "../common/display_path.h"
#include "../fs/directory_sync.h"
#include <map>
#include <variant>

namespace pulse {
namespace {
using S = l10n::StringId;

std::array<app::Tab*, 2> SideBySideTabs(const AppState& s) {
    const auto* layout = LiveLayout(s);
    if (!layout || layout->layout != app::LayoutPreset::TwoVertical || !layout->root) return {};
    const auto& root = *layout->root;
    if (root.is_leaf || root.orientation != app::SplitOrientation::Vertical ||
        !root.first || !root.second || !root.first->is_leaf || !root.second->is_leaf ||
        !root.first->pane || !root.second->pane) return {};
    return {root.first->pane->ActiveTab(), root.second->pane->ActiveTab()};
}

bool FolderUsable(const app::Tab* tab) {
    return tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
        !tab->listing_failed;
}

struct PanePairStamp {
    std::array<uint64_t, 2> ids{}, revisions{};
    std::array<std::wstring, 2> paths;
    bool operator==(const PanePairStamp&) const = default;
};

std::array<uint64_t, 2> PairIds(const std::array<app::Tab*, 2>& tabs) {
    return {tabs[0] ? tabs[0]->network_identity : 0, tabs[1] ? tabs[1]->network_identity : 0};
}

PanePairStamp PairStamp(const std::array<app::Tab*, 2>& tabs) {
    if (!tabs[0] || !tabs[1]) return {};
    return {PairIds(tabs), {tabs[0]->navigation_revision, tabs[1]->navigation_revision},
        {tabs[0]->current_path, tabs[1]->current_path}};
}

struct LinkedRequest {
    PanePairStamp pair;
    size_t from = 0;
    std::wstring target;
    ui::SurfaceMotion motion{};
    std::atomic_bool cancelled = false;
};

struct LinkedResult {
    std::shared_ptr<LinkedRequest> job;
    bool source_exists = false;
    bool target_exists = false;
};

struct SyncRequest {
    PanePairStamp pair;
    fs::DirectorySyncMode mode{};
    std::atomic_bool cancelled = false;
};

struct SyncResult {
    std::shared_ptr<SyncRequest> job;
    fs::DirectorySyncPlan plan;
    bool rechecked = false;
    bool changed = false;
};

using Result = std::variant<LinkedResult, SyncResult>;
}

struct PaneToolsState {
    std::shared_ptr<WindowResultMailbox<Result>> mailbox;
    std::array<uint64_t, 2> link_ids{};
    std::array<std::wstring, 2> link_roots;
    std::shared_ptr<LinkedRequest> link_job;
    std::shared_ptr<SyncRequest> sync_job;
};

namespace {
PaneToolsState& Tools(AppState& s) {
    if (!s.pane_tools) {
        s.pane_tools = std::make_shared<PaneToolsState>();
        s.pane_tools->mailbox = std::make_shared<WindowResultMailbox<Result>>(s.hwnd, WM_PANE_TOOLS_RESULT);
    }
    return *s.pane_tools;
}

template<class Request>
void CancelRequest(std::shared_ptr<Request>& job) {
    if (job) job->cancelled.store(true);
    job.reset();
}

void DisableLink(PaneToolsState& state) {
    CancelRequest(state.link_job);
    state.link_ids = {};
    state.link_roots = {};
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void SyncToast(AppState& s, S message) {
    s.notification_toast.Show(s.hwnd, l10n::Get(S::DirectorySync), l10n::Get(message), false);
}

bool QueueComparison(AppState& s, const std::shared_ptr<SyncRequest>& job,
                     std::optional<fs::DirectorySyncPlan> expected = {}) {
    return s.worker.EnqueueIo([job, mailbox = Tools(s).mailbox, expected = std::move(expected)] {
        if (job->cancelled.load()) return;
        SyncResult result;
        result.job = job;
        result.rechecked = expected.has_value();
        try {
            result.plan = fs::CompareDirectories(job->pair.paths, job->mode, [job] {
                return job->cancelled.load();
            });
            result.changed = expected && *expected != result.plan;
        } catch (...) {
            result.plan.error = ERROR_GEN_FAILURE;
        }
        if (!job->cancelled.load()) mailbox->Publish(std::move(result));
    });
}

std::wstring PlanRow(const fs::DirectorySyncItem& item) {
    using C = fs::SyncChange;
    const auto relative = item.relative.c_str();
    if (item.change == C::Link) return l10n::Format(S::SyncSkipLinkFormat, relative);
    if (item.change == C::TypeConflict) return l10n::Format(S::SyncSkipTypeFormat, relative);
    if (item.change == C::NameConflict) return l10n::Format(S::SyncSkipNameFormat, relative);
    if (item.change == C::SameTimeConflict) return l10n::Format(S::SyncSkipTimeFormat, relative);
    const auto direction = l10n::Get(item.from == 0 ? S::SyncLeftArrowRight : S::SyncRightArrowLeft).c_str();
    if (item.change == C::AddFolder)
        return l10n::Format(S::SyncCopyFolderFormat, direction, relative,
            static_cast<unsigned long long>(item.descendants.size()));
    return l10n::Format(item.change == C::ReplaceFile ? S::SyncConflictFileFormat : S::SyncCopyFileFormat,
        direction, relative);
}

void ApplyCopies(AppState& s, const fs::DirectorySyncPlan& plan) {
    std::map<std::wstring, std::vector<std::wstring>> copies;
    for (const auto& item : plan.items) {
        if (fs::IsDirectorySyncCopy(item.change)) copies[item.destination_dir].push_back(item.source);
    }
    for (auto& [destination, sources] : copies) {
        ops::OpRequest request;
        request.type = ops::OpType::Copy;
        request.dest_dir = destination;
        request.sources = std::move(sources);
        if (!SubmitWithConflictResolution(s, std::move(request))) {
            SyncToast(s, S::SyncQueueFailed);
            break;
        }
    }
}

void ApplySyncResult(AppState& s, PaneToolsState& state, SyncResult result) {
    const auto& job = result.job;
    if (state.sync_job != job || job->cancelled.load()) return;
    if (!IsWindowVisible(s.hwnd) || job->pair != PairStamp(SideBySideTabs(s))) {
        CancelRequest(state.sync_job);
        return;
    }
    auto& plan = result.plan;
    if (plan.error != ERROR_SUCCESS) {
        state.sync_job.reset();
        if (plan.error != ERROR_CANCELLED) {
            const auto detail = path::FriendlyPathText(plan.error_path);
            s.notification_toast.ShowError(s.hwnd, l10n::Get(S::DirectorySync),
                l10n::Format(S::SyncCompareFailedFormat, detail.c_str(), plan.error));
        }
        return;
    }
    if (plan.overlap) {
        state.sync_job.reset();
        SyncToast(s, S::SyncOverlap);
        return;
    }
    if (result.rechecked) {
        state.sync_job.reset();
        if (result.changed) SyncToast(s, S::SyncPlanChanged);
        else ApplyCopies(s, plan);
        return;
    }
    if (plan.items.empty()) {
        state.sync_job.reset();
        SyncToast(s, S::SyncNothingToCopy);
        return;
    }

    const auto count = std::count_if(plan.items.begin(), plan.items.end(), [](const auto& item) {
        return fs::IsDirectorySyncCopy(item.change);
    });
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(S::DirectorySync);
    spec.message = l10n::Format(S::SyncPlanFormat,
        path::FriendlyPathText(job->pair.paths[0]).c_str(), path::FriendlyPathText(job->pair.paths[1]).c_str(),
        static_cast<unsigned long long>(count), static_cast<unsigned long long>(plan.items.size() - count));
    spec.preserve_message = true;
    spec.confirm_text = l10n::Get(count ? S::SyncApply : S::Close);
    spec.hide_cancel = count == 0;
    spec.cancel_is_default = count != 0;
    spec.fit_to_work_area = true;
    spec.still_valid = [job] { return !job->cancelled.load(); };
    // The confirmation view lays out every row. Bound that work for large trees.
    constexpr size_t kVisiblePlanRows = 120;
    for (size_t i = 0; i < std::min(plan.items.size(), kVisiblePlanRows); ++i)
        spec.rows.push_back({PlanRow(plan.items[i]), false, true});
    if (plan.items.size() > kVisiblePlanRows)
        spec.rows.push_back({l10n::Format(S::SyncMoreRowsFormat,
            static_cast<unsigned long long>(plan.items.size() - kVisiblePlanRows)), false, false});
    if (count) spec.note = l10n::Get(S::SyncConflictNote);
    const bool confirmed = ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (job->cancelled.load() || state.sync_job != job) return;
    if (!confirmed || !count || job->pair != PairStamp(SideBySideTabs(s))) {
        state.sync_job.reset();
    } else if (!QueueComparison(s, job, std::move(plan))) {
        state.sync_job.reset();
        SyncToast(s, S::SyncQueueFailed);
    }
}

void ApplyLinkResult(AppState& s, PaneToolsState& state, const LinkedResult& result) {
    const auto& job = result.job;
    if (state.link_job != job || job->cancelled.load()) return;
    state.link_job.reset();
    const auto tabs = SideBySideTabs(s);
    if (job->pair != PairStamp(tabs) || !result.source_exists) return;
    auto& target = *tabs[1 - job->from];
    if (!result.target_exists) {
        s.notification_toast.Show(s.hwnd, l10n::Get(S::LinkedBrowsing),
            l10n::Format(S::LinkedFolderMissingFormat, path::FriendlyPathText(job->target).c_str()), false);
        return;
    }
    if (ActiveTab(s) == &target && (s.addressSearching || s.renameIndex >= 0 || s.filterEditing)) return;
    const auto returned = app::NavigationReturnChildName(target.current_path, job->target);
    if (!target.NavigateTo(job->target, job->motion)) return;
    StartLoadingPath(s, target, job->target, PathLoadReason::LinkedPane);
    RestoreNavigationReturnSelection(s, target, returned);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}

void FillPaneToolsCommandContext(const AppState& s, app::CommandContext& context) {
    const auto tabs = SideBySideTabs(s);
    if (!tabs[0] || !tabs[1]) return;
    context.side_by_side = true;
    const bool folders = FolderUsable(tabs[0]) && FolderUsable(tabs[1]);
    context.link_browsing_on = s.pane_tools && s.pane_tools->link_ids == PairIds(tabs);
    context.can_link_browsing = folders || context.link_browsing_on;
    context.can_sync_directories = folders && !tabs[0]->net_readonly && !tabs[1]->net_readonly &&
        !(s.pane_tools && s.pane_tools->sync_job) &&
        !path::EqualInsensitive(tabs[0]->current_path, tabs[1]->current_path);
}

void ResetInvalidPaneTools(AppState& s) {
    if (!s.pane_tools) return;
    auto& state = *s.pane_tools;
    const auto tabs = SideBySideTabs(s);
    if (state.link_ids != PairIds(tabs)) DisableLink(state);
    if (state.sync_job && state.sync_job->pair != PairStamp(tabs)) CancelRequest(state.sync_job);
}

void ToggleLinkedBrowsing(AppState& s) {
    const auto tabs = SideBySideTabs(s);
    if (!tabs[0] || !tabs[1]) return;
    auto& state = Tools(s);
    const auto ids = PairIds(tabs);
    if (state.link_ids == ids) {
        DisableLink(state);
    } else if (FolderUsable(tabs[0]) && FolderUsable(tabs[1])) {
        DisableLink(state);
        state.link_ids = ids;
        state.link_roots = {tabs[0]->current_path, tabs[1]->current_path};
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SynchronizePaneNavigation(AppState& s, app::Tab& tab) {
    if (!s.pane_tools || !s.pane_tools->link_ids[0]) return;
    auto& state = *s.pane_tools;
    const auto tabs = SideBySideTabs(s);
    if (state.link_ids != PairIds(tabs)) return;
    if (tabs[0] != &tab && tabs[1] != &tab) return;
    const size_t from = tabs[0] == &tab ? 0 : 1;
    CancelRequest(state.link_job);
    auto target = fs::MapLinkedDirectory(state.link_roots, from, tab.current_path);
    if (target.empty()) {
        DisableLink(state);
        return;
    }
    if (path::EqualInsensitive(target, tabs[1 - from]->current_path)) return;
    auto job = std::make_shared<LinkedRequest>();
    job->pair = PairStamp(tabs);
    job->from = from;
    job->target = std::move(target);
    job->motion = tab.navigation_motion;
    if (s.worker.EnqueueIo([job, mailbox = state.mailbox] {
        if (job->cancelled.load()) return;
        const bool source_exists = IsDirectory(job->pair.paths[job->from]);
        if (job->cancelled.load()) return;
        const bool target_exists = source_exists && IsDirectory(job->target);
        if (!job->cancelled.load()) mailbox->Publish(LinkedResult{job, source_exists, target_exists});
    })) state.link_job = std::move(job);
}

void BeginDirectorySync(AppState& s, fs::DirectorySyncMode mode) {
    app::CommandContext context;
    FillPaneToolsCommandContext(s, context);
    if (!context.can_sync_directories) return;
    auto& state = Tools(s);
    auto job = std::make_shared<SyncRequest>();
    job->pair = PairStamp(SideBySideTabs(s));
    job->mode = mode;
    if (!QueueComparison(s, job)) {
        SyncToast(s, S::SyncQueueFailed);
        return;
    }
    state.sync_job = std::move(job);
    SyncToast(s, S::SyncComparing);
}

void ApplyPaneToolsResults(AppState& s, WPARAM identity) {
    const auto state = s.pane_tools;
    if (!state) return;
    for (auto& result : state->mailbox->Take(identity)) {
        if (const auto* linked = std::get_if<LinkedResult>(&result))
            ApplyLinkResult(s, *state, *linked);
        else
            ApplySyncResult(s, *state, std::get<SyncResult>(std::move(result)));
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ClosePaneTools(AppState& s) {
    if (!s.pane_tools) return;
    DisableLink(*s.pane_tools);
    CancelRequest(s.pane_tools->sync_job);
    s.pane_tools->mailbox->Close();
    s.pane_tools.reset();
}
}
