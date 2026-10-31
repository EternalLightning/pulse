// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/frame_profile.h"
#include <dxgi1_2.h>

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
        double interval, input;
        app::FrameProfileSample sample;
        bool view_reused;
        size_t pixel_rows;
    };
    std::vector<Row> rows;
    rows.reserve(1600);
    for (int scenario = 0; scenario < 2; ++scenario) for (int repeat = 0; repeat < 2; ++repeat) {
        Reset(s.renderer);
        tab.filter_text.clear(); tab.view_mode = ViewMode::Details;
        tab.scroll_x = tab.scroll_y = 0; tab.SelectOnly(-1); ++tab.view_generation;
        s.scrollTargetY = 0;
        auto previous = Clock::now();
        int changes = 0;
        for (int frame = -48; frame < 320; ++frame) {
            const auto began = Clock::now();
            const auto input_begin = Clock::now();
            if (frame >= 0 && frame % 32 == 0) {
                if (scenario == 0) SetViewMode(s, changes % 2 ? ViewMode::Details : ViewMode::LargeIcons);
                else { tab.filter_text = changes % 2 ? L"" : L"match"; ++tab.view_generation; }
                ++changes;
            }
            const double input = std::chrono::duration<double, std::milli>(Clock::now() - input_begin).count();
            app::FrameProfileSample sample;
            bool active;
            {
                app::ObserveFrameProfile observe(sample);
                active = Render(s);
            }
            if (!sample.complete || FAILED(sample.draw_result)) {
                std::printf("[FAIL] profile frame scenario=%d frame=%d complete=%d HRESULT=%08lx\n",
                    scenario, frame, sample.complete, static_cast<unsigned long>(sample.draw_result));
                ++failures;
            }
            if (frame >= 0) rows.push_back({scenario, repeat, frame, active,
                std::chrono::duration<double, std::milli>(began - previous).count(), input, sample,
                ViewTransition(s.renderer).ReusedFrame(), Surface(s.renderer).CachedPixelRows()});
            previous = began;
            MSG message{};
            while (PeekMessageW(&message, s.hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (!presented) std::this_thread::sleep_until(began + std::chrono::microseconds(4167));
        }
    }
    std::printf("FRAME_CSV,scenario,repeat,frame,active,interval_ms,input_ms,prepare_ms,paint_ms,submit_ms,present_ms,after_ms,total_ms,presented,view_reused,pixel_rows\n");
    for (const auto& row : rows) {
        const auto& p = row.sample.milliseconds;
        std::printf("FRAME_CSV,%s,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d,%d,%zu\n",
            row.scenario ? "filter" : "view", row.repeat, row.frame, row.active, row.interval, row.input,
            p[0], p[1], p[2], p[3], p[4], row.sample.total_ms, row.sample.presented, row.view_reused, row.pixel_rows);
    }
    for (int scenario = 0; scenario < 2; ++scenario) {
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
            std::sort(values.begin(), values.end());
            std::printf("[FRAME_PROFILE] scenario=%s presented=%d metric=%s frames=%zu p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                scenario ? "filter" : "view", presented, metric, values.size(), values[values.size()/2],
                values[(values.size()*95-1)/100], values[(values.size()*99-1)/100], values.back());
        };
        summary(work, "cpu_work_ms"); summary(interval, "interval_ms"); summary(prepare, "prepare_ms"); summary(paint, "paint_ms");
        std::printf("[PRESENTED] scenario=%d frames=%zu/%zu\n", scenario, submitted, work.size());
        std::printf("[CACHE] scenario=%d view_reused_frames=%zu row_pixel_frames=%zu\n", scenario, reused_frames, pixel_frames);
    }
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    std::printf("[PROFILE_DONE] failures=%d\n", failures);
    return failures ? 1 : 0;
}
