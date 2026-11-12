// SPDX-License-Identifier: Apache-2.0
#pragma once

inline int pulse::ui::ItemReflowUiTest::RunDenseViewModes() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Dense view transition",
        WS_POPUP, -30000, -30000, 3440, 1440, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) {
            check(false, "initialize dense view compositor");
            if (hwnd) DestroyWindow(hwnd);
            return 1;
        }
        auto owned_renderer = std::make_unique<MainRenderer>();
        auto& renderer = *owned_renderer;
        compositor.RecreateTextFormats(1);
        renderer.SetCompositor(&compositor);
        renderer.SetScale(1);
        renderer.SetDetailsPanelVisible(false);
        const auto theme = MakeTheme(false, HexColor(0x0078D4));
        {
            ViewModeTransition transition;
            auto mode = ViewMode::Details;
            const auto bounds = D2D1::RectF(0, 0, 640, 320);
            const auto icon_bounds = [&](int i) {
                const float offset = mode == ViewMode::Details ? 0.0f : 2.0f;
                const float left = 5.0f + (i % 20) * 24.0f + offset;
                const float top = 5.0f + (i / 20) * 20.0f + offset;
                return D2D1::RectF(left, top, left + 12, top + 12);
            };
            const auto frame = [&](uint64_t now) {
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0));
                transition.Paint(dc, L"overflow", mode, bounds, {}, 1, now, true, false, [&] {
                    ComPtr<ID2D1SolidColorBrush> brush;
                    dc->CreateSolidColorBrush(mode == ViewMode::Details ? D2D1::ColorF(1, 0, 0) :
                        D2D1::ColorF(0, 1, 0), &brush);
                    for (int i = 0; i <= static_cast<int>(ViewModeTransition::kMaxIcons); ++i) {
                        const auto icon = icon_bounds(i);
                        transition.Icon(dc, std::to_wstring(i), icon, i, [&] {
                            dc->FillRectangle(icon, brush.get());
                        });
                    }
                }, 0, 1 + static_cast<uint64_t>(mode));
                check(SUCCEEDED(dc->EndDraw()), "overflow frame has no Direct2D error");
            };
            frame(1000);
            int retained = 0;
            transition.RetainIcons([&](int source, float extent) {
                retained += source >= 0 && extent == 12;
            });
            check(retained == static_cast<int>(ViewModeTransition::kMaxIcons) + 1,
                "overflow icons retain thumbnail visibility despite the motion budget");
            mode = ViewMode::MediumIcons;
            frame(1100);
            check(transition.Pending(), "one icon over the budget keeps the transition active");
            frame(1265);
            const auto icon = icon_bounds(static_cast<int>(ViewModeTransition::kMaxIcons));
            const auto pixel_bounds = D2D1::RectF(icon.left + 3, icon.top + 3, icon.left + 4, icon.top + 4);
            const auto faded = view_mode_title::Pixels(compositor, pixel_bounds);
            check(faded.size() == 1 && (faded[0] & 0x00FF0000u) == 0 &&
                (faded[0] & 0x0000FF00u) > 0 && (faded[0] & 0x0000FF00u) < 0x0000FF00u,
                "overflow icon fades with decoration instead of popping or disappearing");
            check(transition.ReusedFrame() && transition.CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
                "overflow preserves bounded reusable icon pictures");
            frame(1320);
            const auto settled = view_mode_title::Pixels(compositor, pixel_bounds);
            check(settled.size() == 1 && (settled[0] & 0x00FFFFFFu) == 0x0000FF00u,
                "settled overflow icon has full opacity");
        }
        for (const int width : {1100, 3440}) {
            const int height = width == 1100 ? 820 : 1440;
            compositor.Resize(width, height);
            const auto rect = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
            for (const int count : {80, 10000}) for (const auto from : {ViewMode::Details, ViewMode::List}) {
                WindowViewModel vm;
                vm.pane.path = L"C:\\pulse-dense-view-fixture";
                vm.pane.header_text = L"View transitions";
                vm.pane.is_file_system = true;
                for (int i = 0; i < count; ++i) {
                    ListEntryView entry;
                    entry.name = L"Folder " + std::to_wstring(i);
                    entry.path = vm.pane.path + L"\\" + entry.name;
                    entry.is_dir = true;
                    entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
                    vm.pane.entries.push_back(std::move(entry));
                }
                const auto frame = [&](uint64_t now) {
                    auto* dc = compositor.Dc();
                    dc->BeginDraw(); dc->Clear(theme.bg);
                    Draw(renderer, vm, rect, theme, now);
                    check(SUCCEEDED(dc->EndDraw()), "dense view frame has no Direct2D error");
                };
                const bool capture = width == 3440 && count == 10000 && from == ViewMode::List;
                const std::filesystem::path output = L".tmp/view-dense-snapshots";
                if (capture) std::filesystem::create_directories(output);
                Reset(renderer);
                vm.pane.view_mode = from;
                frame(1000); frame(1300);
                if (capture) check(WriteAnimationSnapshot(compositor, output / L"start.png"), "save dense source list");
                const auto old_icons = ViewTransition(renderer).CachedIcons();
                vm.pane.view_mode = ViewMode::MediumIcons;
                frame(1400);
                check(ViewTransition(renderer).Pending(), "dense list to icons preserves the transition");
                const auto started = std::chrono::steady_clock::now();
                int reused = 0;
                int stable_frames = 0;
                bool stable_reused = true;
                uint64_t previous_signature = 0;
                const auto signature = [&] {
                    return renderer.ViewModeFrameSignature(vm.pane, theme, true,
                        vm.hover_region, vm.hover_control_index);
                };
                for (uint64_t i = 1; i <= 20; ++i) {
                    const auto before = signature();
                    frame(1400 + i * 8);
                    reused += ViewTransition(renderer).ReusedFrame();
                    const auto after = signature();
                    if (before == after && before == previous_signature) {
                        ++stable_frames;
                        stable_reused &= ViewTransition(renderer).ReusedFrame();
                    }
                    previous_signature = before == after ? after : 0;
                }
                const auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                check(stable_frames > 0 && stable_reused, "dense transition reuses every stable frame");
                std::printf("[DENSE_VIEW] width=%d count=%d from=%ls old_icons=%zu moving_icons=%zu reused=%d cpu_frame_ms=%.3f\n",
                    width, count, ViewModeName(from), old_icons, ViewTransition(renderer).CachedIcons(), reused, elapsed / 20);
                if (capture) {
                    check(WriteAnimationSnapshot(compositor, output / L"middle.png"), "save dense transition midpoint");
                }
                frame(1700);
                if (capture) check(WriteAnimationSnapshot(compositor, output / L"settled.png"), "save dense target grid");
                check(!ViewTransition(renderer).Pending(), "dense transition settles");
                check(ViewTransition(renderer).CachedIcons() <= ViewModeTransition::kMaxIcons,
                    "settled dense view keeps the independent icon budget");
            }
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(hwnd);
    std::printf("dense view fixture: %d failures\n", failures);
    return failures ? 1 : 0;
}
