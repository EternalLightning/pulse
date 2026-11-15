// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"
#include "../app/entry_sort.h"
#include "../ui/lumatext_renderer.h"
#include <future>

inline int pulse::ui::ItemReflowUiTest::RunColdScroll() {
    using namespace pulse;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = s.shot.active = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    wchar_t directory[32768]{};
    const DWORD directory_length = GetEnvironmentVariableW(L"PULSE_TEST_SCROLL_PATH", directory, ARRAYSIZE(directory));
    if (directory_length >= ARRAYSIZE(directory)) return 1;
    const bool real_directory = directory_length != 0;
    wchar_t snapshot_mode[2]{};
    const bool capture_scroll = GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", snapshot_mode, 2) == 1 &&
        snapshot_mode[0] == L'1';
    if (real_directory) {
        s.appPrefs.Load();
        l10n::Initialize(GetModuleHandleW(nullptr), s.appPrefs.language);
    }
    s.scale = 1; s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Cold scroll fixture",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        std::printf("[FAIL] cold scroll compositor\n");
        return 1;
    }
    s.compositor.RecreateTextFormats(1);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1); s.renderer.SetDetailsPanelVisible(false);
    if (real_directory) {
        s.renderer.SetRowHeightDip(static_cast<float>(s.appPrefs.row_height));
        s.renderer.SetListStyle(s.appPrefs.list_smart_date, s.appPrefs.list_zebra_rows, s.appPrefs.list_size_bar);
    }
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = real_directory ? directory : L"C:\\pulse-cold-scroll-fixture";
    tab.group_mode = GroupMode::None;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    const wchar_t* extensions[] = {L".txt", L".pdf", L".cpp", L".zip"};
    for (int i = 0; !real_directory && i < 10000; ++i) {
        fs::DirEntry entry;
        entry.name = L"Document " + std::to_wstring(i) + extensions[i % 4];
        entry.full_path = tab.current_path + L"\\" + entry.name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.size = 1234ull * (i + 1);
        const ULONGLONG stamp = 134385120000000000ull + 600000000ull * i;
        entry.mtime = {static_cast<DWORD>(stamp), static_cast<DWORD>(stamp >> 32)};
        entries->push_back(std::move(entry));
    }
    if (real_directory) {
        const auto started = std::chrono::steady_clock::now();
        try {
            std::async(std::launch::async, [&] {
                if (!fs::EnumerateDirectory(tab.current_path, *entries))
                    throw std::runtime_error("directory enumeration cancelled");
            }).get();
        } catch (const std::exception& error) {
            std::printf("[FAIL] enumerate real scroll directory: %s\n", error.what());
            s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
            return 1;
        }
        const auto sort = s.appPrefs.folder_views.FindSort(tab.current_path).value_or(
            app::FolderSortPrefs{SortColumn::Mtime, SortDirection::Desc});
        tab.sort_column = sort.column; tab.sort_direction = sort.direction;
        const auto group = s.appPrefs.folder_views.FindGroup(tab.current_path).value_or(app::FolderGroupPrefs{});
        tab.group_mode = group.mode; tab.group_direction = group.direction;
        if (const auto columns = s.appPrefs.folder_views.FindColumns(tab.current_path))
            tab.details_column_dividers = *columns;
        tab.show_hidden_files = s.appPrefs.show_hidden_files;
        tab.show_protected_os_files = s.appPrefs.show_protected_os_files;
        std::sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
            return app::EntryLess(a, b, sort.column, sort.direction,
                app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode), tab.current_path);
        });
        std::printf("[DIRECTORY] entries=%zu enumerate_sort_ms=%.3f group=%d row_height=%d\n", entries->size(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
            static_cast<int>(tab.group_mode), s.appPrefs.row_height);
    }
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    if (real_directory) {
        // Warm window chrome before measuring navigation into an uncached directory.
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>()); tab.loading = false;
        for (int i = 0; i < 3; ++i) Render(s);
    }
    tab.SetSnapshot(entries); tab.loading = false;
    int failures = 0;
    const auto vm = BuildVm(s, false);
    const ViewLayout layout(ViewMode::Details, ListRect(s), vm.pane.EntryCount(), 0, 0, s.scale,
        s.renderer.ListRowHeightDip(vm.pane), vm.pane.date_groups.get());
    if (real_directory && capture_scroll) {
        const auto list = ListRect(s);
        s.mouseLeaveTracked = true;
        s.hoverPoint = {static_cast<LONG>(list.left + 80 * s.scale),
            static_cast<LONG>(list.top + s.renderer.RowHeight() * 4.5f)};
    }
    for (int pass = 0; pass < 2; ++pass) {
        const auto* text_stats = s.compositor.GetLumaTextStats();
        const auto text_before = text_stats ? *text_stats : LumaTextStats{};
        std::vector<double> times;
        std::array<double, static_cast<size_t>(profile::PaintPhase::Count)> phases{};
        auto next_frame = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 180; ++frame) {
            if (real_directory) std::this_thread::sleep_until(next_frame);
            tab.scroll_y = real_directory ? layout.MaxScrollY() * frame / 179.0f : frame * 24.0f;
            profile::FrameProfileSample sample;
            { profile::ObserveFrameProfile observe(sample); Render(s); }
            failures += !sample.complete || FAILED(sample.draw_result);
            times.push_back(sample.total_ms - sample.milliseconds[static_cast<size_t>(profile::FramePhase::Present)]);
            for (size_t i = 0; i < phases.size(); ++i) phases[i] += sample.paint_milliseconds[i];
            if (real_directory && capture_scroll && pass == 0 && frame % 60 == 0 &&
                !WriteAnimationSnapshot(s.compositor, L"downloads-scroll-" + std::to_wstring(frame) + L".png"))
                ++failures;
            if (real_directory) {
                if (frame < 5) std::printf("[FIRST_FRAME] pass=%d frame=%d cpu_ms=%.3f prepare_ms=%.3f paint_ms=%.3f submit_ms=%.3f columns_ms=%.3f rows_ms=%.3f\n",
                    pass, frame, times.back(), sample.milliseconds[0], sample.milliseconds[1], sample.milliseconds[2],
                    sample.paint_milliseconds[static_cast<size_t>(profile::PaintPhase::ListColumns)],
                    sample.paint_milliseconds[static_cast<size_t>(profile::PaintPhase::ListRows)]);
                next_frame = std::max(next_frame + std::chrono::microseconds(8333), std::chrono::steady_clock::now());
            }
            MSG message{};
            while (PeekMessageW(&message, s.hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        std::sort(times.begin(), times.end());
        std::printf("[COLD_SCROLL] pass=%d entries=%zu frames=%zu cpu_p50_ms=%.3f cpu_p95_ms=%.3f cpu_max_ms=%.3f columns_total_ms=%.3f rows_total_ms=%.3f\n",
            pass, entries->size(), times.size(), times[times.size()/2], times[times.size()*95/100], times.back(),
            phases[static_cast<size_t>(profile::PaintPhase::ListColumns)], phases[static_cast<size_t>(profile::PaintPhase::ListRows)]);
        if (text_stats) std::printf("[TEXT_CACHE] pass=%d hits=%llu misses=%llu evictions=%llu fallbacks=%llu glyphs=%llu\n",
            pass, text_stats->surface_cache_hits - text_before.surface_cache_hits,
            text_stats->surface_cache_misses - text_before.surface_cache_misses,
            text_stats->surface_cache_evictions - text_before.surface_cache_evictions,
            text_stats->fallback_draws - text_before.fallback_draws,
            text_stats->freetype_glyphs - text_before.freetype_glyphs);
    }
    const bool bounded = tab.view_row_cache && tab.view_row_cache->rows.size() <= 256;
    std::printf("[%s] cold scroll keeps row presentation cache bounded\n", bounded ? "PASS" : "FAIL");
    failures += !bounded;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    std::printf("[SUMMARY] cold scroll failures=%d; %s hidden rendering, excludes display pacing\n", failures,
        real_directory ? "real-directory 120Hz-paced" : "synthetic");
    return failures ? 1 : 0;
}
