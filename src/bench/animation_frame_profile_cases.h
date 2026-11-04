// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_internal.h"
#include "frame_profile_output.h"
#include "gpu_timestamp_profile.h"
#include "presentation_statistics_profile.h"

inline int pulse::ui::ItemReflowUiTest::RunFrameProfile(bool presented) {
    using namespace pulse;
    using Clock = std::chrono::steady_clock;
    int failures = 0;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = s.shot.active = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.scale = 1; s.accentColor = HexColor(0x0078D4); s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"Pulse animation measurement",
        WS_POPUP, presented ? 30 : -30000, presented ? 30 : -30000, 1100, 820,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        std::printf("[FAIL] frame profile compositor initialization\n");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.compositor.RecreateTextFormats(1);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1); s.renderer.SetDetailsPanelVisible(false); s.renderer.motion_enabled_ = true;
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = L"C:\\pulse-frame-profile";
    tab.group_mode = GroupMode::None;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 10000; ++i) {
        fs::DirEntry entry;
        entry.name = (i % 3 ? L"match document " : L"other document ") + std::to_wstring(i);
        entry.full_path = tab.current_path + L"\\" + entry.name;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.is_dir = true;
        entries->push_back(std::move(entry));
    }
    tab.SetSnapshot(entries); tab.loading = false;
    bench::FillProfileSidebar(s, 24);
    bench::ProfileMemory memory;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID2D1Image> image;
        ComPtr<ID2D1Bitmap1> target;
        ComPtr<IDXGISurface> surface;
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIAdapter1> adapter1;
        s.compositor.Dc()->GetTarget(&image);
        if (image && SUCCEEDED(image.As(&target)) && SUCCEEDED(target->GetSurface(&surface)) &&
            SUCCEEDED(surface->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter.As(&adapter1))) {
            DXGI_ADAPTER_DESC1 description{};
            adapter1->GetDesc1(&description);
            adapter.As(&memory.adapter);
            std::printf("[ADAPTER] %ls software=%d\n", description.Description,
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
        } else std::printf("[ADAPTER] unavailable\n");
    }
    MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
    DEVMODEW display{}; display.dmSize = sizeof(display);
    if (GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &display))
        std::printf("[DISPLAY] %ls %lux%lu refresh_hz=%lu presented=%d\n", monitor.szDevice,
            display.dmPelsWidth, display.dmPelsHeight, display.dmDisplayFrequency, presented);
    const auto icon_deadline = Clock::now() + std::chrono::seconds(6);
    bool icons_ready = false;
    do {
        const auto small_icon = s.renderer.icon_cache_.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 16);
        const auto large_icon = s.renderer.icon_cache_.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 256);
        icons_ready = small_icon && large_icon;
        if (!icons_ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!icons_ready && Clock::now() < icon_deadline);
    std::printf("[ICON_READY] native_folder_sizes_16_256=%d\n", icons_ready);
    if (!icons_ready) {
        std::printf("[FAIL] native folder icons unavailable; refuse incomplete animation workload\n");
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return 1;
    }
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", presented ? nullptr : L"1");
    if (presented) ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    struct Row {
        int scenario, repeat, frame;
        bool active;
        double interval, input, wait;
        app::FrameProfileSample sample;
        bool view_reused;
        size_t pixel_rows;
    };
    std::vector<Row> rows;
    rows.reserve(16000);
    constexpr const char* scenarios[] = {"view", "filter", "scroll", "tabs", "tray"};
    std::printf("[FRAME_WORKLOAD] warmup_ms=400 measurement_ms=2000 input_period_ms=240 repeats=2 pacing=natural-render\n");
    std::printf("[FRAME_LIMIT] Present duration is CPU call time; intervals are render-loop intervals, not display timestamps or GPU duration\n");
    std::printf("[FRAME_MODE] %s\n", presented ? "presented-render-workload" : "hidden-render-workload-not-desktop-performance");
    const bool waitable = presented && s.compositor.WaitableFramePacingEnabled();
    wchar_t requested_pacing[16]{};
    const bool requested_waitable = GetEnvironmentVariableW(L"PULSE_FRAME_PACING", requested_pacing, 16) == 8 &&
        std::wstring_view(requested_pacing) == L"waitable";
    std::printf("[FRAME_PACING] requested_waitable=%d enabled=%d\n", requested_waitable, waitable);
    wchar_t queue_depth[2]{};
    const unsigned configured_latency = GetEnvironmentVariableW(L"PULSE_FRAME_LATENCY", queue_depth, 2) == 1 &&
        queue_depth[0] == L'2' ? 2u : 1u;
    std::printf("[FRAME_QUEUE] waitable_latency=%u\n", configured_latency);
    if (presented && requested_waitable && !waitable) {
        std::printf("[FAIL] waitable pacing unavailable; refuse mislabeled comparison\n");
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return 1;
    }
    bench::GpuTimestampProfile gpu_profile;
    const bool gpu_requested = bench::GpuTimestampProfile::Requested();
    if (gpu_requested) {
        const HRESULT result = gpu_profile.Init(s.compositor.Dc());
        if (FAILED(result)) {
            std::printf("[FAIL] requested GPU timestamp initialization HRESULT=%08lx\n", static_cast<unsigned long>(result));
            s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
            return 1;
        }
    }
    app::ObserveFrameDrawProfile observe_gpu(gpu_requested ? gpu_profile.Hooks() : app::FrameDrawProfileHooks{});
    bench::PresentationStatisticsProfile presentation_profile;
    const bool presentation_requested = bench::PresentationStatisticsProfile::Requested();
    if (presentation_requested) presentation_profile.Start(s.compositor);
    int64_t render_id = 0;
    bool frame_permit = false;
    for (int scenario = 0; scenario < 5; ++scenario) for (int repeat = 0; repeat < 2; ++repeat) {
        Reset(s.renderer);
        tab.filter_text.clear(); tab.view_mode = ViewMode::Details;
        tab.scroll_x = tab.scroll_y = 0; tab.SelectOnly(-1); ++tab.view_generation;
        s.scrollTargetY = 0;
        s.tray.Clear(); s.trayCards.clear(); s.trayDrag = {}; s.trayDeckOffset = 0;
        s.trayLastTick = 0; s.trayDeckLastTotal = 0;
        if (scenario == 4) bench::FillProfileTray(s, 32);
        while (s.window_tabs.items.size() > 1) s.window_tabs.CloseTab(s.window_tabs.items.size() - 1);
        s.window_tabs.SwitchTab(0);
        s.pane = s.window_tabs.Active()->FocusedPane();
        const auto started = Clock::now();
        auto previous = started;
        auto next_input = started + std::chrono::milliseconds(400);
        const auto until = started + std::chrono::milliseconds(2400);
        int changes = 0;
        int frame = 0;
        while (Clock::now() < until) {
            const auto waiting_begin = Clock::now();
            if (waitable && !frame_permit) {
                HANDLE handle = s.compositor.FrameLatencyWaitableObject();
                for (;;) {
                    const DWORD result = MsgWaitForMultipleObjectsEx(1, &handle, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    if (result == WAIT_OBJECT_0) { frame_permit = true; break; }
                    if (result != WAIT_OBJECT_0 + 1) {
                        std::printf("[FAIL] frame latency wait result=%lu\n", result);
                        ++failures;
                        break;
                    }
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message); DispatchMessageW(&message);
                    }
                }
            }
            const auto began = Clock::now();
            const double waited = std::chrono::duration<double, std::milli>(began - waiting_begin).count();
            if (began >= until) break;
            const auto input_begin = Clock::now();
            if (began >= next_input) {
                if (scenario == 0) SetViewMode(s, changes % 2 ? ViewMode::Details : ViewMode::LargeIcons);
                else if (scenario == 1) { tab.filter_text = changes % 2 ? L"" : L"match"; ++tab.view_generation; }
                else if (scenario == 2) {
                    const auto bounds = FocusedPaneRect(s);
                    POINT point{static_cast<LONG>((bounds.left + bounds.right) * 0.5f),
                        static_cast<LONG>((bounds.top + bounds.bottom) * 0.5f)};
                    ClientToScreen(s.hwnd, &point);
                    const short delta = static_cast<short>(changes % 8 < 4 ? -WHEEL_DELTA : WHEEL_DELTA);
                    HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)),
                        MAKELPARAM(point.x, point.y));
                } else if (scenario == 3) {
                    CaptureTabStripLayout(s);
                    if (changes % 2 == 0) {
                        auto& added = s.window_tabs.NewTab(tab.current_path);
                        added.FocusedPane()->view.SetSnapshot(entries);
                        added.FocusedPane()->view.loading = false;
                        s.window_tabs.SwitchTab(0);
                    } else s.window_tabs.CloseTab(s.window_tabs.items.size() - 1);
                    s.hitTestVmValid = false;
                } else {
                    if (s.trayDrag.active) ReleaseTrayDrag(s, true);
                    const auto top = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)), 1);
                    if (!top.empty()) {
                        s.trayDrag.active = true;
                        s.trayDrag.path = top.front().item->path;
                        s.trayDrag.last_t = TrayAnimationNow();
                    }
                }
                ++changes;
                // Input cadence is elapsed time, independent of the number of rendered samples.
                next_input = began + std::chrono::milliseconds(240);
            }
            if (scenario == 4 && s.trayDrag.active) {
                const double gesture_ms = std::chrono::duration<double, std::milli>(
                    began - (next_input - std::chrono::milliseconds(240))).count();
                s.trayDrag.dx = static_cast<float>(std::min(gesture_ms, 160.0) * 0.8);
                s.trayDrag.dy = static_cast<float>(20 * std::sin(gesture_ms * 0.01));
                s.trayDrag.vx = 0.8f;
                s.trayDrag.last_t = TrayAnimationNow();
                if (gesture_ms >= 160) ReleaseTrayDrag(s, true);
            }
            const double input = std::chrono::duration<double, std::milli>(Clock::now() - input_begin).count();
            const bool measurement = began - started >= std::chrono::milliseconds(400);
            if (gpu_requested) gpu_profile.SetFrame(scenario, repeat, measurement ? frame : -1, measurement);
            app::FrameProfileSample sample;
            bool active;
            {
                app::ObserveFrameProfile observe(sample);
                active = Render(s);
            }
            if (presentation_requested) presentation_profile.Sample(s.compositor, "after-render", render_id++,
                scenario, repeat, measurement ? frame : -1, measurement, sample.presented);
            frame_permit = false;
            if (!sample.complete || FAILED(sample.draw_result)) {
                std::printf("[FAIL] profile frame scenario=%d frame=%d complete=%d HRESULT=%08lx\n",
                    scenario, frame, sample.complete, static_cast<unsigned long>(sample.draw_result));
                ++failures;
            }
            if (measurement) rows.push_back({scenario, repeat, frame++, active,
                std::chrono::duration<double, std::milli>(began - previous).count(), input, waited, sample,
                ViewTransition(s.renderer).ReusedFrame(), Surface(s.renderer).CachedPixelRows()});
            previous = began;
            memory.Sample(scenarios[scenario], repeat);
            MSG message{};
            while (PeekMessageW(&message, s.hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
    }
    if (presentation_requested) presentation_profile.Sample(s.compositor, "final", render_id - 1, -1, -1, -1, false, false);
    std::printf("FRAME_CSV,scenario,repeat,frame,active,interval_ms,input_ms,prepare_ms,paint_ms,submit_ms,present_ms,after_ms,total_ms,presented,view_reused,pixel_rows,wait_ms,%s,%s\n", bench::vm_csv_columns, bench::paint_csv_columns);
    for (const auto& row : rows) {
        const auto& p = row.sample.milliseconds;
        std::printf("FRAME_CSV,%s,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%zu,%.6f",
            scenarios[row.scenario], row.repeat, row.frame, row.active, row.interval, row.input,
            p[0], p[1], p[2], p[3], p[4], row.sample.total_ms, row.sample.presented, row.view_reused, row.pixel_rows, row.wait);
        bench::PrintVmProfile(row.sample);
        bench::PrintPaintProfile(row.sample);
        std::printf("\n");
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        std::vector<double> work, interval, prepare, paint;
        size_t submitted = 0;
        size_t reused_frames = 0, pixel_frames = 0;
        for (const auto& row : rows) if (row.scenario == scenario) {
            work.push_back(row.sample.total_ms - row.sample.milliseconds[3]); interval.push_back(row.interval);
            prepare.push_back(row.sample.milliseconds[0]); paint.push_back(row.sample.milliseconds[1]);
            submitted += row.sample.presented;
            reused_frames += row.view_reused;
            pixel_frames += row.pixel_rows > 0;
        }
        const auto summary = [&](std::vector<double>& values, const char* metric) {
            if (values.empty()) { ++failures; return; }
            std::sort(values.begin(), values.end());
            std::printf("[FRAME_PROFILE] scenario=%s presented=%d metric=%s frames=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                scenarios[scenario], presented, metric, values.size(), values[(values.size()-1)/2],
                values[(values.size()*95-1)/100], values[(values.size()*99-1)/100], values.back());
        };
        summary(work, "cpu_work_ms"); summary(interval, "interval_ms"); summary(prepare, "prepare_ms"); summary(paint, "paint_ms");
        std::printf("[PRESENTED] scenario=%d frames=%zu/%zu\n", scenario, submitted, work.size());
        std::printf("[CACHE] scenario=%d view_reused_frames=%zu row_pixel_frames=%zu\n", scenario, reused_frames, pixel_frames);
    }
    if (gpu_requested) gpu_profile.Print(scenarios, std::size(scenarios));
    if (presentation_requested) presentation_profile.Print(scenarios, std::size(scenarios));
    memory.Print();
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    failures += memory.failures;
    std::printf("[PROFILE_DONE] failures=%d\n", failures);
    return failures ? 1 : 0;
}

inline int pulse::ui::ItemReflowUiTest::RunVmProfile() {
    using namespace pulse;
    using Clock = std::chrono::steady_clock;
    constexpr int warmup_samples = 32, measured_samples = 1024, repeats = 3;
    struct Case { const char* name; int sidebar, tabs, tray; };
    constexpr Case cases[] = {
        {"normal", 24, 1, 0}, {"sidebar", 256, 1, 0},
        {"tabs", 256, 48, 0}, {"tray", 256, 48, 2048}
    };
    struct Row { const char* scenario; int repeat, sample; double total_ms; app::FrameProfileSample profile; };
    std::vector<Row> rows;
    rows.reserve(std::size(cases) * measured_samples * repeats);
    bench::ProfileMemory memory;
    int failures = 0;
    size_t checksum = 0;
    std::printf("[VM_PROFILE_CPU_ONLY] BuildVm only; no compositor, rendering, native Shell icons or display pacing\n");
    std::printf("[VM_WORKLOAD] warmup_samples=%d measured_samples=%d repeats=%d file_entries=10000 compositor_size=0x0\n",
        warmup_samples, measured_samples, repeats);
    for (const auto& test : cases) {
        auto owned = std::make_unique<AppState>();
        auto& s = *owned;
        s.isolatedTest = s.shot.active = true;
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.scale = 1; s.accentColor = HexColor(0x0078D4); s.showDetailsPanel = false;
        s.renderer.SetScale(1); s.renderer.SetDetailsPanelVisible(false);
        s.window_tabs.EnsureDefault();
        s.pane = s.window_tabs.Active()->FocusedPane();
        auto& tab = *ActiveTab(s);
        tab.current_path = L"C:\\pulse-frame-profile";
        tab.group_mode = GroupMode::None;
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        entries->reserve(10000);
        for (int i = 0; i < 10000; ++i) {
            fs::DirEntry entry;
            entry.name = L"Profile folder " + std::to_wstring(i);
            entry.full_path = tab.current_path + L"\\" + entry.name;
            entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
            entry.is_dir = true;
            entries->push_back(std::move(entry));
        }
        tab.SetSnapshot(entries); tab.loading = false;
        bench::FillProfileSidebar(s, test.sidebar);
        for (int i = 1; i < test.tabs; ++i) {
            auto& added = s.window_tabs.NewTab(tab.current_path);
            added.FocusedPane()->view.SetSnapshot(entries);
            added.FocusedPane()->view.loading = false;
        }
        s.window_tabs.SwitchTab(0);
        bench::FillProfileTray(s, test.tray);
        std::printf("[VM_CASE] scenario=%s sidebar_entries=%zu tabs=%zu tray_entries=%d file_entries=%zu\n",
            test.name, s.sidebar.quick_access.size(), s.window_tabs.items.size(), TrayItemTotalCount(s.tray), entries->size());
        for (int repeat = 0; repeat < repeats; ++repeat) {
            memory.Sample(test.name, repeat);
            for (int sample = -warmup_samples; sample < measured_samples; ++sample) {
                app::FrameProfileSample profile;
                const auto started = Clock::now();
                {
                    app::ObserveFrameProfile observe(profile);
                    const auto vm = BuildVm(s);
                    checksum += vm.tabs.size() + vm.pane.EntryCount() + vm.tray_deck.cards.size();
                    if (sample == 0 && (vm.tabs.size() != static_cast<size_t>(test.tabs) ||
                        vm.pane.EntryCount() != entries->size() || vm.tray_deck.total_count != test.tray)) {
                        std::printf("[FAIL] VM workload model dimensions scenario=%s repeat=%d\n", test.name, repeat);
                        ++failures;
                    }
                }
                const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
                if (sample >= 0) {
                    rows.push_back({test.name, repeat, sample, elapsed, profile});
                    if (profile.vm_builds != 1 || profile.vm_ms <= 0) ++failures;
                }
                memory.Sample(test.name, repeat);
            }
        }
    }
    std::printf("VM_CSV,scenario,repeat,sample,total_ms,%s\n", bench::vm_csv_columns);
    for (const auto& row : rows) {
        std::printf("VM_CSV,%s,%d,%d,%.6f", row.scenario, row.repeat, row.sample, row.total_ms);
        bench::PrintVmProfile(row.profile);
        std::printf("\n");
    }
    for (const auto& test : cases) {
        std::vector<double> times;
        for (const auto& row : rows) if (row.scenario == test.name) times.push_back(row.profile.vm_ms);
        std::sort(times.begin(), times.end());
        std::printf("[VM_PROFILE] scenario=%s metric=vm_ms samples=%zu p50=%.6f p95=%.6f p99=%.6f max=%.6f\n",
            test.name, times.size(), times[(times.size()-1)/2], times[(times.size()*95-1)/100],
            times[(times.size()*99-1)/100], times.back());
    }
    memory.Print();
    failures += memory.failures;
    std::printf("[VM_PROFILE_DONE] failures=%d checksum=%zu\n", failures, checksum);
    return failures ? 1 : 0;
}
