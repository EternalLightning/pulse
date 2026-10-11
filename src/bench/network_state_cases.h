#pragma once
#include "../app/app_internal.h"
#include "../app/app_column_view.h"
#include "../common/localization.h"
#include "../app/net_cache_presentation.h"
#include <condition_variable>
#include <mutex>
#include <cstdio>

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

inline int RunNetworkStateCases(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    auto* tab = ActiveTab(s);
    const auto path = fs::NormalizePath(L"\\\\network-state.invalid\\share");
    tab->current_path = path;
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>(1);
    snapshot->front().name = L"cached.txt";
    tab->SetSnapshot(snapshot);
    app::WorkResult cached{};
    cached.path = path; cached.generation = 11; cached.snapshot = snapshot;
    cached.sort_column = tab->sort_column; cached.sort_direction = tab->sort_direction;
    cached.folder_sort = app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode);
    cached.final = false; cached.projection_only = true; cached.cache_unix = 1234567890;
    tab->SetSnapshot(nullptr); tab->pending_generation = cached.generation;
    tab->applied_generation = 0; tab->loading = true;
    ApplyWorkerResult(s, cached);
    check(tab->snapshot == snapshot && !tab->loading && tab->pending_generation == cached.generation &&
        tab->cache_unix == cached.cache_unix && tab->banner_type == app::BannerType::Other &&
        tab->banner_title == l10n::Get(l10n::StringId::Cache) &&
        tab->banner_message.find(app::FormatCacheAge(cached.cache_unix)) != std::wstring::npos &&
        !s.store.Peek(path),
        "disk provisional shows its cache age and keeps enumeration pending without freshening SnapshotStore");
    auto rejected_cache = cached;
    auto obsolete = std::make_shared<std::vector<fs::DirEntry>>(1);
    obsolete->front().name = L"obsolete-disk.txt";
    rejected_cache.snapshot = obsolete; rejected_cache.cache_unix = 1;
    for (int mismatch = 0; mismatch < 5; ++mismatch) {
        auto result = rejected_cache;
        if (mismatch == 0) result.path += L"\\other";
        if (mismatch == 1) --result.generation;
        if (mismatch == 2) result.sort_column = tab->sort_column == ui::SortColumn::Name
            ? ui::SortColumn::Size : ui::SortColumn::Name;
        if (mismatch == 3) result.sort_direction = tab->sort_direction == ui::SortDirection::Asc
            ? ui::SortDirection::Desc : ui::SortDirection::Asc;
        if (mismatch == 4) result.folder_sort = cached.folder_sort == app::FolderSortMode::Mixed
            ? app::FolderSortMode::FoldersFirst : app::FolderSortMode::Mixed;
        ApplyWorkerResult(s, result);
        check(tab->snapshot == snapshot && tab->cache_unix == cached.cache_unix &&
            tab->pending_generation == cached.generation,
            "provisional with a different path, generation, sort, direction or folder mode is rejected");
    }
    tab->column_layout = true;
    tab->column_strip = {};
    tab->column_strip.for_path = path;
    app::ColumnStripColumn ancestor;
    ancestor.path = path; ancestor.generation = cached.generation; ancestor.requested = true;
    tab->column_strip.ancestors.push_back(ancestor);
    ancestor.generation = cached.generation + 1; ancestor.snapshot = obsolete;
    tab->column_strip.ancestors.push_back(ancestor);
    ancestor.path = path + L"\\unrelated-column"; ancestor.generation = cached.generation;
    tab->column_strip.ancestors.push_back(ancestor);
    tab->SetSnapshot(nullptr); tab->loading = true;
    ApplyWorkerResult(s, cached);
    check(tab->snapshot == snapshot && tab->pending_generation == cached.generation &&
        tab->column_strip.ancestors[0].snapshot == snapshot &&
        tab->column_strip.ancestors[0].generation == cached.generation &&
        tab->column_strip.ancestors[0].requested &&
        tab->column_strip.ancestors[1].snapshot == obsolete &&
        tab->column_strip.ancestors[1].generation == cached.generation + 1 &&
        tab->column_strip.ancestors[2].snapshot == obsolete &&
        tab->column_strip.ancestors[2].generation == cached.generation,
        "disk provisional reaches its main list and matching ancestor while preserving pending columns and rejecting other paths or generations");
    auto newer_cache = rejected_cache;
    newer_cache.generation = cached.generation + 2;
    ApplyWorkerResult(s, newer_cache);
    check(tab->snapshot == snapshot && tab->column_strip.ancestors[0].snapshot == snapshot &&
        tab->column_strip.ancestors[0].generation == cached.generation &&
        tab->column_strip.ancestors[1].snapshot == obsolete &&
        tab->column_strip.ancestors[1].generation == cached.generation + 1,
        "ancestor provisional requires its exact pending generation even when the incoming cache generation is newer");
    auto current = std::make_shared<std::vector<fs::DirEntry>>(1);
    current->front().name = L"current-online.txt";
    auto complete = cached;
    complete.final = true; complete.projection_only = false;
    complete.snapshot = current; complete.facts = current; complete.cache_unix = 0;
    ApplyWorkerResult(s, complete);
    check(tab->snapshot == current && !tab->pending_generation && !tab->loading &&
        !tab->cache_unix && tab->banner_type == app::BannerType::None && !tab->net_readonly &&
        s.store.Peek(path) == current,
        "final success replaces disk rows and clears the cache banner and age");
    check(tab->column_strip.ancestors[0].snapshot == current &&
        !tab->column_strip.ancestors[0].generation && tab->column_strip.ancestors[0].requested &&
        tab->column_strip.ancestors[0].applied_generation == cached.generation &&
        tab->column_strip.ancestors[1].snapshot == obsolete &&
        tab->column_strip.ancestors[2].snapshot == obsolete,
        "final success replaces matching ancestor cache rows and retires only its pending generation");
    ApplyWorkerResult(s, cached);
    check(tab->snapshot == current && !tab->pending_generation && !tab->cache_unix &&
        tab->banner_type == app::BannerType::None && s.store.Peek(path) == current,
        "late same-generation disk provisional cannot overwrite successful final rows or restore the cache banner");
    check(tab->column_strip.ancestors[0].snapshot == current &&
        !tab->column_strip.ancestors[0].generation &&
        tab->column_strip.ancestors[0].applied_generation == cached.generation,
        "late disk provisional cannot replace an ancestor after its successful final");
    tab->column_layout = false;
    tab->column_strip = {};
    tab->SetSnapshot(snapshot);
    const auto deliver = [&](uint64_t id, fs::NetStatus status, DWORD error, bool final = true) {
        auto result = std::make_unique<fs::UncProbeResult>();
        result->probe_id = id; result->unc = path; result->status = status;
        result->error_code = error; result->final = final;
        WndProcImpl(s.hwnd, WM_NET_PROBE, 0, reinterpret_cast<LPARAM>(result.release()));
    };
    auto a = s.probe_scheduler.Begin(path);
    deliver(a, fs::NetStatus::Offline, ERROR_TIMEOUT, false);
    check(tab->net_readonly && tab->banner_type == app::BannerType::Offline,
        "latest logical timeout marks its held snapshot offline");
    const auto b = s.probe_scheduler.Begin(path);
    deliver(b, fs::NetStatus::Online, ERROR_SUCCESS);
    deliver(a, fs::NetStatus::Offline, ERROR_BAD_NETPATH);
    check(!tab->net_readonly && tab->banner_type == app::BannerType::None,
        "old late probe cannot overwrite newer online UI state");
    a = s.probe_scheduler.Begin(path);
    deliver(a, fs::NetStatus::Offline, ERROR_TIMEOUT, false);
    deliver(a, fs::NetStatus::Online, ERROR_SUCCESS);
    check(!tab->net_readonly && tab->banner_type == app::BannerType::None,
        "late success without a replacement remains usable");
    a = s.probe_scheduler.Begin(path);
    deliver(a, fs::NetStatus::Unknown, ERROR_ACCESS_DENIED);
    check(tab->net_readonly && tab->banner_type == app::BannerType::Other && !tab->banner_message.empty(),
        "access denied preserves read-only snapshot with an error rather than an offline banner");
    const auto denied_message = tab->banner_message;
    a = s.probe_scheduler.Begin(path);
    deliver(a, fs::NetStatus::Unknown, ERROR_OPERATION_ABORTED);
    check(tab->net_readonly && tab->banner_type == app::BannerType::Other &&
        tab->banner_message == denied_message,
        "cancelled probe preserves the previous connectivity and error state");
    tab->ClearBanner(); tab->net_readonly = false;
    app::WorkResult failure;
    failure.path = path; failure.error = true; failure.error_code = ERROR_ACCESS_DENIED;
    failure.generation = 51; tab->pending_generation = 51; tab->loading = true;
    ApplyWorkerResult(s, failure);
    check(!tab->loading && !tab->pending_generation && tab->banner_type == app::BannerType::Other && tab->net_readonly,
        "UNC enumeration permission failure has a typed non-offline terminal");
    failure.error_code = ERROR_BAD_NETPATH; failure.generation = 52;
    tab->pending_generation = 52; tab->loading = true;
    ApplyWorkerResult(s, failure);
    check(!tab->loading && tab->banner_type == app::BannerType::Offline,
        "UNC connectivity failure retains the offline terminal");
    failure.error_code = ERROR_GEN_FAILURE; failure.generation = 53;
    tab->pending_generation = 53; tab->loading = true;
    ApplyWorkerResult(s, failure);
    check(!tab->loading && tab->banner_type == app::BannerType::Other,
        "unknown provider error does not masquerade as a disconnect");
    s.worker.Stop();
    wchar_t cwd[32768]{}; GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);
    const auto local = fs::NormalizePath(std::wstring(cwd) + L"\\bench_data\\network-admission-fixture");
    StartLoadingPath(s, *tab, local);
    check(!tab->loading && !tab->pending_generation && tab->banner_type == app::BannerType::Other,
        "rejected local navigation cannot remain in loading state");
    app::WorkResult stale;
    stale.path = local;
    stale.generation = tab->applied_generation - 1;
    stale.snapshot = snapshot;
    ApplyWorkerResult(s, stale);
    check(!tab->loading && !tab->pending_generation && tab->banner_type == app::BannerType::Other &&
        tab->snapshot != snapshot, "queued old enumeration cannot erase a newer rejection terminal");
    tab->SetSnapshot(nullptr);
    RefreshPath(s, local, RefreshReason::Explicit);
    check(!tab->loading && !tab->pending_generation && tab->banner_type == app::BannerType::Other &&
        !s.explicit_entry_refreshes.contains(tab), "rejected explicit refresh clears its pending request and loading state");
    for (const auto view : {L"pulse:starred", L"pulse:recent", L"pulse:recycle"}) {
        LoadVirtualView(s, *tab, view);
        check(!tab->loading && !tab->pending_generation && tab->banner_type == app::BannerType::Other,
            "rejected virtual listing has the same admission terminal as local navigation");
    }
    const auto column_path = local + L"-column";
    tab->current_path = fs::NormalizePath(column_path + L"\\child");
    tab->column_layout = true;
    tab->column_strip = {};
    tab->column_strip.for_path = tab->current_path;
    app::ColumnStripColumn column;
    column.path = column_path;
    tab->column_strip.ancestors.push_back(std::move(column));
    SyncColumnStrips(s);
    const auto& rejected = tab->column_strip.ancestors.front();
    check(rejected.requested && rejected.error && !rejected.generation && rejected.applied_generation,
        "rejected column enumeration has an error terminal and advances its result watermark");
    tab->column_layout = false;
    tab->column_strip = {};

    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        unsigned entered = 0;
        bool release = false;
    };
    const auto gate = std::make_shared<Gate>();
    fs::SetUncProbeHookForTest([gate](const std::wstring&) {
        std::unique_lock lock(gate->mutex);
        ++gate->entered; gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
        return true;
    }, 40);
    bool started = true;
    for (unsigned i = 0; i < 4; ++i)
        started &= fs::StartUncProbe(s.hwnd, WM_NET_PROBE, path, app::UncProbeScheduler::NextId());
    {
        std::unique_lock lock(gate->mutex);
        started &= gate->changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered == 4; });
    }
    const auto drain = [&] {
        MSG msg{};
        while (PeekMessageW(&msg, s.hwnd, WM_NET_PROBE, WM_NET_PROBE, PM_REMOVE))
            WndProcImpl(s.hwnd, msg.message, msg.wParam, msg.lParam);
    };
    const auto timeout_deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < timeout_deadline && fs::UncProbeResourcesForTest().retired < 4) {
        drain(); Sleep(5);
    }
    drain();
    tab->current_path = path;
    tab->net_readonly = true;
    tab->SetBannerTitle(app::BannerType::Offline, L"offline");
    RequestUncProbe(s, path);
    const auto* pending = s.probe_scheduler.Pending();
    const auto pending_id = pending ? pending->id : 0;
    check(started && fs::UncProbeResourcesForTest().retired == 4 && pending_id &&
        !s.probe_scheduler.active_id, "fifth UI probe remains queued while four actual owners are retired");
    {
        std::lock_guard lock(gate->mutex); gate->release = true;
    }
    gate->changed.notify_all();
    const auto resume_deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < resume_deadline) {
        drain();
        if (fs::UncProbeResourcesForTest().live == 0 && s.probe_scheduler.queued.empty() &&
            !s.probe_scheduler.active_id) break;
        Sleep(5);
    }
    drain();
    check(pending_id && s.probe_scheduler.queued.empty() && !s.probe_scheduler.active_id &&
        !s.probe_scheduler.latest.contains(path) && !tab->net_readonly &&
        tab->banner_type == app::BannerType::None && fs::UncProbeResourcesForTest().live == 0,
        "actual capacity release wakes the UI and completes its queued probe without another request");
    fs::SetUncProbeHookForTest({});
    s.watches.Stop();
    return failures ? 1 : 0;
}
