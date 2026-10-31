// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"
#include "../app/app_ui_schedule.h"

namespace pulse::index {
struct ContentRefreshTestPeer {
    static void SetScope(ContentSearchClient& client, const std::wstring& root) {
        std::lock_guard lock(client.state_mu_);
        client.config_.roots = {{root, text::Encoding::Auto}};
        client.config_ready_ = true;
        client.running_ = true; // Capture requests without starting a provider thread.
    }
    static bool FreshRequest(ContentSearchClient& client) {
        std::lock_guard lock(client.state_mu_);
        return !client.pending_requests_.empty() && !client.pending_requests_.back().previous_results;
    }
};
}

inline int pulse::ui::ItemReflowUiTest::RunSearchFilter(const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Search and filter updates",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize isolated search/filter compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.compositor.RecreateTextFormats(1);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring root = L"C:\\pulse-search-filter-fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto* name : {L"alpha.txt", L"budget.txt", L"budget-old.txt", L"notes.txt"}) {
        fs::DirEntry entry;
        entry.name = name; entry.full_path = root + L"\\" + name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entries->push_back(std::move(entry));
    }
    tab.current_path = root;
    tab.SetSnapshot(entries);
    const auto pane = [&] {
        PaneViewModel vm;
        app::FillPaneViewModel(vm, *s.pane, &s.places);
        return vm;
    };
    const auto frame = [&](uint64_t now, const wchar_t* label, bool dark = false, float scale = 1) {
        s.renderer.SetScale(scale);
        s.compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(820 * scale));
        s.compositor.RecreateTextFormats(scale);
        WindowViewModel vm;
        vm.dark = dark; vm.pane = pane();
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        auto* dc = s.compositor.Dc();
        dc->BeginDraw(); dc->Clear(theme.bg);
        DrawSearchFilter(s.renderer, vm, D2D1::RectF(0, 0, 1100 * scale, 820 * scale), theme, now);
        check(SUCCEEDED(dc->EndDraw()), "production pane draws without Direct2D errors");
        check(!s.renderer.pane_surfaces_[0].pending, "typing never starts a whole-pane transition");
        if (label) check(WriteAnimationSnapshot(s.compositor, output / label), "save search/filter animation frame");
    };
    ShowFilterEditor(s);
    s.filterFocusPending = false;
    s.pane->filter_expand = 1;
    frame(1000, L"filter-before.png");
    const auto type_filter = [&](const wchar_t* text) {
        SetWindowTextW(s.hwndFilterEdit, text);
        SyncFilterEditor(s);
        return s.filterDue;
    };
    const auto first_due = type_filter(L"b");
    check(first_due >= GetTickCount64() + 240, "filter waits approximately 250 ms after input");
    check(!TickFilterEditor(s, first_due - 1) && pane().EntryCount() == 4,
        "filter leaves the published rows unchanged before its deadline");
    Sleep(20);
    const auto due = type_filter(L"budget");
    check(due > first_due && !TickFilterEditor(s, first_due), "each keystroke postpones the filter deadline");
    check(tab.filter_text.empty() && pane().EntryCount() == 4, "rapid filter input does not publish intermediate text");
    SyncUiScheduling(s);
    check(s.ui_schedule.deadline == due, "UI scheduler arms the filter deadline");
    check(RunUiDeadlines(s, due) && pane().EntryCount() == 2, "UI deadline applies the final filter text");
    frame(1100, L"filter-remove-0.png");
    check(Surface(s.renderer).Pending(), "filter removal animates surviving rows");
    frame(1180, L"filter-remove-80.png");
    frame(1360, nullptr);
    check(!Surface(s.renderer).Pending(), "filter motion settles");
    SendMessageW(s.hwndFilterEdit, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto ime_due = type_filter(L"alpha");
    check(!TickFilterEditor(s, ime_due + 1000) && tab.filter_text == L"budget",
        "IME composition cannot publish a partial filter");
    SendMessageW(s.hwndFilterEdit, WM_IME_ENDCOMPOSITION, 0, 0);
    check(!s.filterComposing && s.filterDue && s.ui_schedule.deadline == s.filterDue,
        "ending IME composition rearms the filter deadline immediately");
    type_filter(L"");
    HideFilterEditor(s, true);
    check(tab.filter_text.empty() && !s.filterDue, "Enter flushes the pending filter immediately");
    s.pane->filter_expand = 0;
    frame(1450, L"filter-clear-0.png");
    check(Surface(s.renderer).Pending(), "clearing a filter animates additions");
    frame(1710, nullptr);
    ShowFilterEditor(s); s.filterFocusPending = false;
    const auto stale_due = type_filter(L"notes");
    ++tab.view_generation;
    check(!TickFilterEditor(s, stale_due) && tab.filter_text.empty(), "stale filter deadlines cannot change another view");
    HideFilterEditor(s, false); s.pane->filter_expand = 0;
    ShowFilterEditor(s); s.filterFocusPending = false;
    type_filter(L"notes");
    HideFilterEditor(s, false);
    check(!s.filterDue && tab.filter_text.empty(), "Escape discards unpublished filter input");
    s.pane->filter_expand = 0;

    s.hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 300, 30, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.addressSearching = s.addressEditing = true;
    s.addressSearchRoot = root;
    s.addressSearchCurrent = true;
    const auto type_search = [&](const wchar_t* text) {
        SetWindowTextW(s.hwndAddressEdit, text);
        QueueAddressSearch(s);
        return s.addressLiveDue;
    };
    const auto publish = [&](uint32_t id, std::initializer_list<size_t> rows, DWORD error = ERROR_SUCCESS) {
        index::SearchResult result;
        for (auto row : rows) {
            const auto& entry = (*entries)[row];
            result.hits.push_back({entry.full_path, entry.name});
        }
        result.total = result.hits.size(); result.error = error;
        AcceptIndexProviderResult(s, id, std::move(result), false);
    };
    const auto search_due = type_search(L"b");
    check(search_due >= GetTickCount64() + 240, "search uses the same 250 ms pause");
    TickAddressSearchDeadlines(s, search_due - 1);
    check(s.pendingIndexSearches.empty() && tab.current_path == root, "search does not run before the input pause");
    type_search(L"budget"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    const auto first = static_cast<uint32_t>(tab.pending_generation);
    publish(first, {1, 2});
    for (const float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
        for (auto& surface : s.renderer.pane_surfaces_) surface.Reset();
        Reset(s.renderer);
        const auto suffix = std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light");
        frame(2000, nullptr, dark, scale);
        type_search(L"budget-old");
        const auto old = tab.snapshot;
        check(tab.snapshot == old && pane().EntryCount() == 2, "typing keeps existing search results during debounce");
        TickAddressSearchDeadlines(s, s.addressLiveDue);
        const auto next = static_cast<uint32_t>(tab.pending_generation);
        check(tab.snapshot == old && tab.search_retaining_results && pane().EntryCount() == 2,
            "a pending search replacement keeps its published rows");
        check(tab.CountBound() == 0, "retained rows cannot be acted on as current-query matches");
        frame(2100, nullptr, dark, scale);
        publish(first, {0});
        check(tab.pending_generation == next && pane().EntryCount() == 2, "late results from an older query are ignored");
        publish(next, {2});
        check(!tab.loading && !tab.search_retaining_results && pane().EntryCount() == 1,
            "only replacement-query matches become actionable");
        frame(2200, (L"search-change-0-" + suffix + L".png").c_str(), dark, scale);
        check(Surface(s.renderer).Pending(), "changed search results use item animation");
        frame(2280, (L"search-change-80-" + suffix + L".png").c_str(), dark, scale);
        frame(2460, nullptr, dark, scale);
        check(!Surface(s.renderer).Pending(), "search item animation settles");
        type_search(L"budget"); TickAddressSearchDeadlines(s, s.addressLiveDue);
        publish(static_cast<uint32_t>(tab.pending_generation), {1, 2});
        frame(2600, nullptr, dark, scale);
        frame(2780, (L"search-expand-180-" + suffix + L".png").c_str(), dark, scale);
        frame(2860, (L"search-settled-" + suffix + L".png").c_str(), dark, scale);
    }
    type_search(L"missing"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    publish(static_cast<uint32_t>(tab.pending_generation), {});
    check(!tab.loading && pane().EntryCount() == 0, "empty replacement removes every stale search row");
    type_search(L"budget"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    publish(static_cast<uint32_t>(tab.pending_generation), {1, 2});
    type_search(L"denied"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    publish(static_cast<uint32_t>(tab.pending_generation), {}, ERROR_ACCESS_DENIED);
    check(!tab.loading && pane().EntryCount() == 0 && tab.banner_type == app::BannerType::SearchIncomplete,
        "failed replacement clears old matches and shows its error");

    index::ContentRefreshTestPeer::SetScope(s.contentSearch, root);
    s.addressSearchContent = true;
    type_search(L"first body"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    const auto content_first = tab.pending_generation;
    check(content_first && index::ContentRefreshTestPeer::FreshRequest(s.contentSearch),
        "a changed content query starts a fresh producer without reusing old matches");
    const auto store = [&](std::initializer_list<size_t> rows) {
        auto result = std::make_shared<index::ContentResultStore>(nullptr, 0);
        std::vector<index::ContentHit> hits;
        for (auto row : rows) hits.push_back({(*entries)[row].full_path, (*entries)[row].name, L"body match"});
        check(result->Append(hits), "prepare isolated content result spool");
        result->Prefetch(0);
        const auto until = GetTickCount64() + 2000;
        while (!result->Ready(0) && GetTickCount64() < until) Sleep(5);
        check(result->Ready(0), "visible content rows are ready without UI-thread disk reads");
        return result;
    };
    const auto publish_content = [&](uint64_t generation, std::shared_ptr<index::ContentResultStore> results,
                                     DWORD error = ERROR_SUCCESS, bool done = true) {
        index::ContentSearchUpdate update;
        update.progress.generation = generation; update.progress.done = done; update.progress.error = error;
        update.results = std::move(results);
        ApplyContentSearchUpdate(s, std::move(update));
    };
    auto first_store = store({1, 2});
    publish_content(content_first, first_store);
    RefreshContentResults(s);
    for (auto& surface : s.renderer.pane_surfaces_) surface.Reset();
    Reset(s.renderer);
    frame(3000, nullptr);
    type_search(L"second body"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    const auto content_second = tab.pending_generation;
    check(tab.content_results == first_store && tab.search_retaining_results && pane().EntryCount() == 2,
        "content query replacement retains the published store");
    publish_content(content_second, nullptr, ERROR_SUCCESS, false);
    RefreshContentResults(s);
    check(tab.content_results == first_store && tab.search_retaining_results && tab.loading,
        "content progress notifications do not replace retained results");
    frame(3100, nullptr);
    auto second_store = store({2});
    publish_content(content_second, second_store);
    RefreshContentResults(s);
    check(tab.content_results == second_store && pane().EntryCount() == 1 && !tab.search_retaining_results,
        "new content results replace the previous store");
    frame(3200, L"content-change-0.png");
    check(Surface(s.renderer).Pending(), "content results animate by stable file identity");
    frame(3280, L"content-change-80.png"); frame(3460, nullptr);
    type_search(L"failed body"); TickAddressSearchDeadlines(s, s.addressLiveDue);
    publish_content(tab.pending_generation, nullptr, ERROR_ACCESS_DENIED);
    check(!tab.content_results && pane().EntryCount() == 0 && !tab.loading,
        "content failure cannot expose another query's rows as current matches");

    s.watches.Stop();
    DestroyWindow(s.hwnd);
    s.hwndAddressEdit = s.hwndFilterEdit = s.hwnd = nullptr;
    std::printf("search_filter_failures=%d\n", failures);
    return failures ? 1 : 0;
}
