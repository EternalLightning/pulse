// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/cloud_commands.h"
#include "../app/cloud_roots.h"
#include "../app/cloud_sync_probe.h"
#include "../app/change_time.h"

namespace pulse::bench {
template<class Check>
void StatusRepaintCases(AppState& s, Check check) {
    uint32_t provider = 0;
    unsigned queries = 0;
    app::CloudSyncProbe probe([&](const std::wstring&) { ++queries; return provider; },
        app::CloudSyncProbe::Mode::Manual);
    probe.Lookup(L"fixture", 1);
    probe.RunPendingForTest();
    check(probe.Revision() == 0, "initial neutral cloud result does not change displayed status");
    const auto poll = [&] {
        probe.Lookup(L"fixture", 1, GetTickCount64() + 10001);
        check(probe.QueuedForTest() == 1, "idle cloud polling still queues provider work without painting");
        probe.RunPendingForTest();
    };
    poll();
    check(queries == 2 && probe.Revision() == 0, "repeated neutral cloud result needs no repaint");
    provider = 6;
    poll();
    const auto syncing = probe.Revision();
    check(syncing == 1, "background transition to syncing requests a repaint");
    check(probe.ChangedSince(L"fixture", 0) && !probe.ChangedSince(L"unseen", 0),
        "cloud revisions identify only the file whose displayed status changed");
    poll();
    check(probe.Revision() == syncing, "unchanged syncing status needs no repaint");
    check(probe.Lookup(L"fixture", 2) == fs::CloudSync::Idle,
        "modified cloud row temporarily displays neutral state while querying");
    probe.RunPendingForTest();
    check(probe.Revision() == syncing + 1,
        "same syncing result restores the badge after modification invalidates it");
    provider = 7;
    probe.Lookup(L"fixture", 2, GetTickCount64() + 10001);
    probe.RunPendingForTest();
    check(probe.Revision() == syncing + 2, "background transition to error requests a repaint");
    provider = 0;
    probe.Lookup(L"fixture", 2, GetTickCount64() + 10001);
    probe.RunPendingForTest();
    check(probe.Revision() == syncing + 3, "background completion requests a repaint");

    s.hitTestVm = BuildVm(s);
    s.hitTestVmValid = true;
    check(std::any_of(s.hitTestVm.pane_slots.begin(), s.hitTestVm.pane_slots.end(),
        [](const auto& slot) { return slot.pane.cloud_status_column; }),
        "production cloud timer fixture contains a visible cloud status column");
    check(ActiveTab(s)->snapshot && !ActiveTab(s)->snapshot->empty(),
        "production cloud timer fixture has a loaded visible file");
    TickCloudStatus(s);
    bool cloud_quiet = true;
    for (int i = 0; i < 3; ++i) {
        s.cloud_status_tick = GetTickCount64() - app::CloudSyncProbe::kSyncingRecheckMs;
        cloud_quiet &= !TickCloudStatus(s);
    }
    check(cloud_quiet, "production cloud timer does not dirty an unchanged cloud folder");
    app::CloudSyncProbe::Instance().Lookup(s.open_path + L"\\unseen.txt", 1);
    check(!TickCloudStatus(s), "offscreen cloud status changes do not repaint the current window");

    auto& state = s.changes;
    s.appPrefs.change_tracking_enabled = true;
    s.appPrefs.change_tracking_days = state.summary_days = 7;
    auto* tab = ActiveTab(s);
    const auto refresh_view = [&] { s.hitTestVm = BuildVm(s); };
    refresh_view();
    check(!state.visible_badges.empty(), "real pane records visible change badges for comparison");
    state.summary_request = 41;
    state.requested_paths = {tab->current_path};
    index::ChangeSummary summary;
    summary.path = tab->current_path;
    summary.state = index::ChangeState::Available;
    index::ChangeResponse result;
    result.summaries = {summary};
    check(!ApplyChangeSummaries(s, 41, result), "successful empty summary does not repaint an empty badge");
    summary.count = 1;
    summary.counts[1] = 1;
    summary.last_change = app::ChangeNow();
    result.summaries = {summary};
    check(ApplyChangeSummaries(s, 41, result), "new visible change badge requests a repaint");
    refresh_view();
    check(!ApplyChangeSummaries(s, 41, result), "identical summary does not repaint");
    summary.last_change -= 1;
    summary.count = 2;
    summary.counts[1] = 2;
    result.summaries = {summary};
    check(!ApplyChangeSummaries(s, 41, result), "hidden tooltip changes do not repaint an unchanged badge");
    s.changes.hover_path = tab->current_path;
    s.changes.popover.visible = true;
    s.changes.popover.summary = L"old tooltip";
    check(ApplyChangeSummaries(s, 41, result) && s.changes.popover.summary != L"old tooltip",
        "visible change popover refreshes when its content changes");
    check(!ApplyChangeSummaries(s, 41, result), "identical visible popover does not repaint");
    s.changes.popover.visible = false;
    summary.has_deleted = true;
    result.summaries = {summary};
    check(ApplyChangeSummaries(s, 41, result), "new deletion marker requests a repaint");
    refresh_view();
    auto failed = summary;
    failed.count = 0;
    failed.last_change = 0;
    failed.state = index::ChangeState::Unavailable;
    result.summaries = {failed};
    check(!ApplyChangeSummaries(s, 41, result), "failed query preserves the badge without repainting");
    failed.state = index::ChangeState::Available;
    result.summaries = {failed};
    check(!ApplyChangeSummaries(s, 40, result), "stale summary cannot request a repaint");
    check(ApplyChangeSummaries(s, 41, result), "successful empty result removes a visible badge");
    refresh_view();
    check(!ApplyChangeSummaries(s, 41, result), "repeated empty summary does not repaint");
    summary.path += L"\\offscreen";
    state.requested_paths.push_back(summary.path);
    result.summaries = {summary};
    check(!ApplyChangeSummaries(s, 41, result), "offscreen summary updates cache without repainting");

    // An expired displayed badge must still disappear if provider queries fail.
    summary.path = tab->current_path;
    result.summaries = {summary};
    ApplyChangeSummaries(s, 41, result);
    refresh_view();
    state.summaries.at(state.visible_badges.begin()->first).last_change = app::ChangeNow() - 8 * 86400;
    check(TickChangeTracking(s), "maintenance detects time-dependent badge disappearance without a reply");
    refresh_view();
    check(!TickChangeTracking(s), "unchanged maintenance does not repaint after badge removal");
    s.appPrefs.change_tracking_enabled = false;
    check(!ApplyChangeSummaries(s, 41, result), "disabled tracking ignores in-flight summaries");

    s.appPrefs.change_tracking_enabled = true;
    state.visible_badges.clear();
    const auto folder = tab->current_path;
    tab->current_path = L"pulse:changes:" + folder;
    tab->loading = false;
    index::ChangeResponse details;
    details.state = index::ChangeState::Available;
    index::ChangeRecord record;
    record.path = folder + L"\\sample.txt";
    record.kind = index::ChangeKind::Modified;
    record.source = index::ChangeSource::Event;
    record.time = app::ChangeNow();
    details.records = {record};
    uint32_t request = 80;
    const auto apply_details = [&](const index::ChangeResponse& response) {
        tab->change_request = ++request;
        state.details[request] = {tab, tab->current_path, tab->view_generation, false};
        return ApplyChangeDetails(s, request, response);
    };
    check(apply_details(details), "new change-detail rows request a repaint");
    const auto original = tab->snapshot;
    check(!apply_details(details) && tab->snapshot == original,
        "identical detail result preserves the snapshot and does not repaint");
    LoadChangeView(s, *tab, folder, false, true);
    const auto slow_request = tab->change_request;
    check(!tab->loading, "background detail refresh does not introduce a loading indicator");
    const bool isolated = s.isolatedTest;
    s.isolatedTest = false;
    state.last_detail_refresh = GetTickCount64() - 5001;
    TickChangeTracking(s);
    s.isolatedTest = isolated;
    check(tab->change_request == slow_request,
        "slow background detail refresh is not replaced by another timer query");
    check(!ApplyChangeDetails(s, slow_request, details), "unchanged background details do not repaint");
    details.records[0].kind = index::ChangeKind::Deleted;
    check(apply_details(details) && tab->snapshot != original && (*tab->snapshot)[0].change_deleted,
        "changed detail event updates rows and requests a repaint");
    index::ChangeResponse unavailable;
    check(apply_details(unavailable), "first failed detail refresh displays its error");
    check(!apply_details(unavailable), "repeated detail failure does not repaint an unchanged error");
    details.records.clear();
    check(apply_details(details), "successful empty details remove rows and stale error");
    check(!apply_details(details), "identical empty detail result does not repaint");
    check(!ApplyChangeDetails(s, slow_request, details), "late detail response does not repaint");
    tab->current_path = folder;
    s.appPrefs.change_tracking_enabled = false;
}
}
