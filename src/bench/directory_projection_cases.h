// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_internal.h"
#include "../app/directory_projection.h"
#include <condition_variable>
#include <mutex>
#include <cstdio>

inline int RunDirectoryProjectionCases(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        failures += !passed;
    };
    auto& tab = *ActiveTab(s);
    const auto path = fs::NormalizePath(L"C:\\pulse-projection-fixture");
    tab.current_path = path;
    auto facts = std::make_shared<std::vector<fs::DirEntry>>(2);
    facts->at(0).name = L"a.txt"; facts->at(0).size = 10;
    facts->at(1).name = L"b.txt"; facts->at(1).size = 1;
    for (auto& entry : *facts) entry.attrs = FILE_ATTRIBUTE_NORMAL;
    tab.sort_column = ui::SortColumn::Name;
    tab.sort_direction = ui::SortDirection::Asc;
    tab.SetSnapshot(facts); tab.SelectOnly(1);
    tab.git_root = L"saved-git-root"; tab.cache_unix = 123; tab.net_readonly = true;
    tab.SetBannerTitle(app::BannerType::Offline, L"offline");
    s.store.Put(path, facts);
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false, entered = false;
    std::vector<app::WorkResult> results;
    s.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(mutex);
        results.push_back(std::move(result)); changed.notify_all();
    });
    app::WorkerCancellationTestProbe::SetCheckpoint(s.worker, [&] {
        std::unique_lock lock(mutex);
        entered = true; changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    SetSort(s, ui::SortColumn::Size, ui::SortDirection::Asc);
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    check(entered && tab.snapshot == facts && tab.pending_generation &&
        tab.EntryAt(0).name == L"a.txt", "sort command returns while its background projection is held");
    const auto generation = tab.pending_generation;
    auto second_pane = std::make_unique<app::Pane>();
    auto& second_tab = second_pane->view;
    second_tab.current_path = path;
    second_tab.sort_column = ui::SortColumn::Name;
    second_tab.sort_direction = ui::SortDirection::Asc;
    second_tab.SetSnapshot(facts); second_tab.SelectOnly(0);
    auto* first_pane = s.pane;
    s.pane = second_pane.get();
    s.window_tabs.Active()->panes.push_back(std::move(second_pane));
    app::RebuildLayoutRoot(*s.window_tabs.Active());
    SetSort(s, ui::SortColumn::Size, ui::SortDirection::Asc);
    check(second_tab.pending_generation == generation,
        "two panes requesting the same pending pure projection share its publication generation");
    s.pane = first_pane;
    {
        std::lock_guard lock(mutex); release = true;
    }
    changed.notify_all();
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return !results.empty(); });
    }
    s.worker.Stop();
    for (auto& result : results) ApplyWorkerResult(s, result);
    check(!results.empty() && !tab.pending_generation && tab.applied_generation == generation &&
        tab.EntryAt(0).name == L"b.txt" && tab.EntryAt(tab.selected_index).name == L"b.txt" &&
        facts->at(0).name == L"a.txt", "background sort swaps an immutable projection and follows the exact selection");
    check(!second_tab.pending_generation && second_tab.snapshot == tab.snapshot &&
        second_tab.EntryAt(second_tab.selected_index).name == L"a.txt",
        "shared pure projection completes both panes and preserves their independent selection");
    check(tab.git_root == L"saved-git-root" && tab.cache_unix == 123 && tab.net_readonly &&
        tab.banner_type == app::BannerType::Offline && s.store.Peek(path) == facts,
        "pure sort preserves source freshness, Git metadata and cached read-only state");
    const auto accepted = tab.snapshot;
    app::WorkResult foreign{};
    foreign.path = path; foreign.generation = generation + 1; foreign.snapshot = facts;
    foreign.sort_column = ui::SortColumn::Name;
    ApplyWorkerResult(s, foreign);
    check(tab.snapshot == accepted, "another pane's sort projection cannot replace this pane's ordering");

    // A sort change must retain the terminal outcome of an active fresh listing.
    results.clear(); entered = release = false;
    tab.sort_column = ui::SortColumn::Name;
    tab.sort_direction = ui::SortDirection::Asc;
    tab.pending_generation = 0;
    s.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(mutex);
        results.push_back(std::move(result)); changed.notify_all();
    });
    app::WorkerCancellationTestProbe::SetCheckpoint(s.worker, [&] {
        std::unique_lock lock(mutex);
        entered = true; changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    tab.pending_generation = s.worker.Refresh(path, tab.sort_column, tab.sort_direction);
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    s.explicit_entry_refreshes[&tab] = {path, tab.pending_generation};
    SetSort(s, ui::SortColumn::Size, ui::SortDirection::Desc);
    const auto fresh_generation = tab.pending_generation;
    check(fresh_generation && s.explicit_entry_refreshes.at(&tab).generation == fresh_generation,
        "sort subscription carries the active explicit refresh ownership forward");
    {
        std::lock_guard lock(mutex); release = true;
    }
    changed.notify_all();
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return std::any_of(results.begin(), results.end(), [&](const auto& result) {
                return result.final && result.generation == fresh_generation;
            });
        });
    }
    s.worker.Stop();
    const auto terminal = std::find_if(results.begin(), results.end(), [&](const auto& result) {
        return result.final && result.generation == fresh_generation;
    });
    check(entered && terminal != results.end() && terminal->error && !terminal->projection_only,
        "sorting during a pending enumeration keeps its filesystem error terminal");
    for (auto& result : results) ApplyWorkerResult(s, result);
    check(!tab.pending_generation && tab.snapshot && tab.banner_type == app::BannerType::Other,
        "sort subscription preserves cached rows when the active enumeration fails");

    tab.current_path = fs::NormalizePath(L"\\\\projection-cache.invalid\\share");
    tab.sort_column = ui::SortColumn::Name;
    tab.sort_direction = ui::SortDirection::Asc;
    tab.SetSnapshot(nullptr); tab.pending_generation = fresh_generation + 1;
    tab.applied_generation = 0; tab.loading = true;
    tab.SetBannerTitle(app::BannerType::Other, L"cache");
    app::WorkResult cached{};
    cached.path = tab.current_path; cached.generation = tab.pending_generation;
    cached.snapshot = facts; cached.final = false;
    ApplyWorkerResult(s, cached);
    check(tab.snapshot == facts && !tab.loading && tab.pending_generation == cached.generation &&
        tab.banner_type == app::BannerType::Other,
        "background cache projection displays rows while the real enumeration stays pending");
    app::WorkResult failure = cached;
    failure.final = true; failure.snapshot.reset(); failure.error = true;
    failure.error_code = ERROR_BAD_NETPATH;
    ApplyWorkerResult(s, failure);
    check(tab.snapshot == facts && tab.net_readonly && !tab.pending_generation &&
        tab.banner_type == app::BannerType::Offline,
        "final enumeration failure preserves the already projected cache with its error terminal");
    ApplyWorkerResult(s, cached);
    check(tab.snapshot == facts && tab.net_readonly && tab.banner_type == app::BannerType::Offline,
        "late provisional cache cannot erase the final error state");
    tab.current_path = path;
    tab.sort_column = ui::SortColumn::Size;
    tab.sort_direction = ui::SortDirection::Asc;
    auto created = std::make_shared<std::vector<fs::DirEntry>>(3);
    created->at(0).name = L"A.txt"; created->at(0).size = 1;
    created->at(1).name = L"keep.txt"; created->at(1).size = 2;
    created->at(2).name = L"a.txt"; created->at(2).size = 10;
    for (auto& entry : *created) entry.attrs = FILE_ATTRIBUTE_NORMAL;
    tab.SetSnapshot(created); tab.pending_generation = 0;
    tab.pending_created_name = L"a.txt"; tab.pending_created_names = {L"a.txt"};
    RevealPendingCreatedItem(s, tab);
    check(tab.snapshot == created && tab.EntryAt(tab.selected_index).name == L"a.txt" &&
        tab.pending_created_name.empty(),
        "created-item reveal selects the exact name without cloning or sorting the published projection");
    tab.pending_created_name = L"a.txt"; tab.pending_created_names = {L"A.txt", L"a.txt"};
    RevealPendingCreatedItem(s, tab);
    check(tab.snapshot == created && tab.SelectedCount() == 2 &&
        tab.EntryAt(tab.selected_index).name == L"a.txt",
        "multi-item reveal preserves both physical case identities in the existing projection");
    s.watches.Stop();
    return failures ? 1 : 0;
}
