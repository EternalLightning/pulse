// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pulse::ui::view_mode_title {
inline std::vector<uint32_t> Pixels(Compositor& compositor, D2D1_RECT_F bounds) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, sample;
    auto* dc = compositor.Dc();
    dc->GetTarget(&image);
    if (!image || FAILED(image.As(&target))) return {};
    const D2D1_RECT_U region{static_cast<UINT32>(bounds.left), static_cast<UINT32>(bounds.top),
        static_cast<UINT32>(bounds.right), static_cast<UINT32>(bounds.bottom)};
    const UINT32 width = region.right - region.left, height = region.bottom - region.top;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &sample)) ||
        FAILED(sample->CopyFromBitmap(nullptr, target.Get(), &region))) return {};
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    for (UINT32 row = 0; row < height; ++row)
        std::memcpy(pixels.data() + static_cast<size_t>(row) * width, mapped.bits + row * mapped.pitch, width * sizeof(uint32_t));
    sample->Unmap();
    return pixels;
}
}

inline int pulse::ui::ItemReflowUiTest::RunViewModes(const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    wchar_t pixel_cache[4]{};
    const bool pixels_enabled = GetEnvironmentVariableW(L"PULSE_ANIMATION_PIXEL_CACHE", pixel_cache, ARRAYSIZE(pixel_cache)) == 1 && pixel_cache[0] == L'1';
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    wchar_t transport[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", transport, ARRAYSIZE(transport)) != 1 || transport[0] != L'1')
        std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"View mode transitions",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) {
            check(false, "initialize view-mode compositor");
            if (hwnd) DestroyWindow(hwnd);
            return 1;
        }
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        renderer.SetDetailsPanelVisible(false);
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            renderer.SetScale(scale);
            compositor.Resize(static_cast<int>(1100 * scale), static_cast<int>(820 * scale));
            compositor.RecreateTextFormats(scale);
            auto rect = D2D1::RectF(0, 0, 1100 * scale, 820 * scale);
            auto theme = MakeTheme(dark, HexColor(0x0078D4));
            WindowViewModel vm;
            vm.dark = dark;
            vm.pane.path = L"C:\\pulse-view-mode-fixture";
            vm.pane.header_text = L"View transitions";
            vm.pane.is_file_system = true;
            for (int i = 0; i < 80; ++i) {
                ListEntryView entry;
                entry.name = L"Folder " + std::to_wstring(i);
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.is_dir = true;
                entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
                vm.pane.entries.push_back(std::move(entry));
            }
            const auto fixture_entries = vm.pane.entries;
            const auto key = vm.pane.entries[3].path + L"\n" + vm.pane.entries[3].name;
            const auto draw = [&](uint64_t now, const wchar_t* label = nullptr, bool animate = true) {
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg);
                renderer.motion_enabled_ = animate;
                renderer.surface_animating_ = false;
                renderer.settings_switch_now_ = now;
                ++renderer.settings_switch_frame_;
                renderer.UpdateBrushes(theme);
                renderer.painter_.BeginFrame(theme, false);
                {
                    ThumbnailCache::FrameScope thumbnails(renderer.thumbnail_cache_);
                    renderer.DrawPane(vm, rect, theme);
                }
                const auto result = dc->EndDraw();
                if (FAILED(result)) std::printf("Direct2D HRESULT: %08lx\n", static_cast<unsigned long>(result));
                check(SUCCEEDED(result), "view-mode frame draws without Direct2D errors");
                if (label) {
                    const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                        (scale == 1 ? L"100-" : L"150-") + label + L".png";
                    check(WriteAnimationSnapshot(compositor, output / name), "save view-mode frame");
                }
            };
            for (int old = 0; old < 8; ++old) for (int next = 0; next < 8; ++next) {
                if (old == next) continue;
                Reset(renderer);
                vm.pane.view_mode = ViewModeFromIndex(old);
                vm.pane.view_mode_direct = false;
                vm.pane.scroll_x = vm.pane.scroll_y = 0;
                draw(1000);
                const auto before = ViewTransition(renderer).IconBounds(key);
                check(!ViewTransition(renderer).Pending(), "initial view has no entry animation");
                vm.pane.view_mode = ViewModeFromIndex(next);
                ++vm.pane.view_generation;
                draw(1100);
                const auto start = ViewTransition(renderer).IconBounds(key);
                check(ViewTransition(renderer).Pending(), "all view-mode pairs start an icon transition");
                check(!renderer.pane_surfaces_[0].pending, "view switch avoids a second whole-pane fade");
                check(std::fabs(start.left - before.left) < 0.01f && std::fabs(start.top - before.top) < 0.01f &&
                    std::fabs(start.right - before.right) < 0.01f, "icon starts at its old position and size");
                draw(1210);
                const auto middle = ViewTransition(renderer).IconBounds(key);
                const auto body = renderer.ContentRect(rect.right, rect.bottom);
                check(renderer.ItemFromPointInPane(vm.pane, body, (middle.left + middle.right) / 2,
                    (middle.top + middle.bottom) / 2) == 3, "moving icon hit test keeps live file identity");
                check(ViewTransition(renderer).CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
                    "retained drawing is bounded by visible icons");
                draw(1320);
                const auto end = ViewTransition(renderer).IconBounds(key);
                check(std::fabs(middle.left - (start.left + end.left) * 0.5f) < 0.1f &&
                    std::fabs(middle.top - (start.top + end.top) * 0.5f) < 0.1f &&
                    std::fabs(middle.right - (start.right + end.right) * 0.5f) < 0.1f,
                    "midpoint interpolates icon position and size");
                check(!ViewTransition(renderer).Pending(), "view animation releases its frame request at 220ms");
            }
            Reset(renderer);
            vm.pane.view_mode = ViewMode::Details;
            draw(1500);
            const auto title_pane = renderer.ContentRect(rect.right, rect.bottom);
            const auto title_crop = D2D1::RectF(title_pane.left, title_pane.top,
                title_pane.left + 200 * scale, title_pane.top + renderer.PaneHeaderHeight());
            const auto title_pixels = view_mode_title::Pixels(compositor, title_crop);
            check(!title_pixels.empty(), "capture folder-title pixels for view-switch regression");
            for (int step = 0; step < 16; ++step) {
                vm.pane.view_mode = ViewModeFromIndex(step < 8 ? step : 15 - step);
                ++vm.pane.view_generation;
                draw(1508 + step * 8);
                check(view_mode_title::Pixels(compositor, title_crop) == title_pixels &&
                    !renderer.pane_title_surfaces_[0].pending,
                    "rapid forward/reverse view changes keep folder-title pixels fixed");
            }
            Reset(renderer);
            vm.pane.view_mode = ViewMode::Details;
            draw(2000, L"details-before");
            vm.pane.view_mode = ViewMode::MediumIcons;
            draw(2100, L"icons-start"); draw(2210, L"icons-middle");
            const auto sampled = ViewTransition(renderer).IconBounds(key);
            vm.pane.view_mode = ViewMode::LargeIcons;
            draw(2210);
            const auto restarted = ViewTransition(renderer).IconBounds(key);
            check(std::fabs(restarted.left - sampled.left) < 0.01f && std::fabs(restarted.top - sampled.top) < 0.01f,
                "rapid switching restarts at the last displayed icon position");
            draw(2430, L"large-end");
            vm.pane.view_mode = ViewMode::Details;
            draw(2500); draw(2610, L"details-return-middle"); draw(2720, L"details-return-end");

            // Outgoing rows remain actionable even when the new sparse grid
            // places them outside the viewport.
            const auto outgoing_key = vm.pane.entries[12].path + L"\n" + vm.pane.entries[12].name;
            const auto outgoing = ViewTransition(renderer).IconBounds(outgoing_key);
            vm.pane.view_mode = ViewMode::ExtraLargeIcons;
            draw(2800); draw(2830);
            check(ViewTransition(renderer).HitTest((outgoing.left + outgoing.right) / 2,
                (outgoing.top + outgoing.bottom) / 2) == 12, "outgoing icon keeps its live file identity");
            draw(3020);

            // Grouped Details replaces filter_map with display order. The
            // transition still pairs pictures by file identity.
            Reset(renderer);
            auto order = std::make_shared<PaneViewModel::FilterMap>();
            auto reverse = std::make_shared<PaneViewModel::FilterMap>(80);
            for (int i = 0; i < 80; ++i) { order->push_back(79 - i); (*reverse)[79 - i] = i; }
            vm.pane.view_mode = ViewMode::Details;
            vm.pane.filter_map = order; vm.pane.source_to_view = reverse;
            draw(3100);
            vm.pane.view_mode = ViewMode::MediumIcons;
            vm.pane.filter_map.reset(); vm.pane.source_to_view.reset();
            draw(3200);
            check(ViewTransition(renderer).Pending(), "grouped display mapping permits a view transition");
            draw(3420);

            // Compare warmed CPU frame submission costs. This deliberately
            // does not claim GPU latency or a guaranteed display frame rate.
            double captured_ms = 0, plain_ms = 0, moving_ms = 0;
            for (int pass = 0; pass < 3; ++pass) {
                if (pass == 2) draw(3900);
                for (int i = 0; i < 12; ++i) {
                    const auto start = std::chrono::steady_clock::now();
                    if (pass == 2) {
                        vm.pane.view_mode = i == 0 ? ViewMode::LargeIcons : vm.pane.view_mode;
                        draw(4000 + i * 16);
                    } else draw(3500 + i, nullptr, pass == 0);
                    const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    if (i >= 2) (pass == 0 ? captured_ms : pass == 1 ? plain_ms : moving_ms) += elapsed;
                }
            }
            std::printf("[METRIC] %s %.0f%% CPU frame submission: captured %.3fms, plain %.3fms, moving %.3fms\n",
                dark ? "dark" : "light", scale * 100, captured_ms / 10, plain_ms / 10, moving_ms / 10);
            draw(4500);
            vm.pane.entries.erase(vm.pane.entries.begin() + 1);
            ++vm.pane.view_generation;
            draw(4600);
            check(Surface(renderer).Pending() && !ViewTransition(renderer).Pending(),
                "same-mode deletion uses the existing reflow animation");
            draw(4670, L"reflow-middle");
            check(pixels_enabled ? Surface(renderer).CachedPixelRows() > 0 : Surface(renderer).CachedPixelRows() == 0,
                "moving icon cells honor the configured pixel-cache policy");
            draw(4840, L"reflow-after");
            check(ViewTransition(renderer).CachedIcons() > 0, "icons remain available after reflow settles");

            vm.pane.view_mode = ViewMode::MediumIcons; draw(5000);
            vm.pane.scroll_y = 30 * scale; draw(5050);
            check(!ViewTransition(renderer).Pending(), "scrolling cancels stale geometry");
            vm.pane.view_mode = ViewMode::LargeIcons; draw(5100);
            ++vm.pane.view_generation; draw(5150);
            check(!ViewTransition(renderer).Pending(), "refresh cancels a pending transition");
            vm.pane.view_mode = ViewMode::MediumIcons; draw(5200);
            vm.pane.path += L"\\Child"; draw(5250);
            check(!ViewTransition(renderer).Pending(), "navigation drops the previous folder's icons");
            vm.pane.view_mode_direct = true;
            vm.pane.view_mode = ViewMode::LargeIcons; draw(5300);
            check(ViewTransition(renderer).Pending(), "wheel manipulation animates individual icons");
            vm.pane.view_mode_direct = false;
            vm.pane.view_mode = ViewMode::MediumIcons; draw(5400);
            draw(5450, nullptr, false);
            check(!ViewTransition(renderer).Pending() && ViewTransition(renderer).CachedIcons() == 0,
                "reduced motion releases captured drawing");
            draw(5500);
            vm.pane.view_mode = ViewMode::Details; draw(5600);
            vm.pane.rename_index = 0; draw(5650);
            check(!ViewTransition(renderer).Pending(), "rename cancels icon motion");
            vm.pane.rename_index = -1;
            vm.pane.loading = true; draw(5700);
            check(!ViewTransition(renderer).Pending() && ViewTransition(renderer).CachedIcons() == 0,
                "loading clears captured icons");
            vm.pane.loading = false; draw(5800);
            vm.pane.view_mode = ViewMode::MediumIcons; draw(5900);
            rect.right = 650 * scale;
            compositor.Resize(static_cast<int>(rect.right), static_cast<int>(rect.bottom));
            draw(5950);
            check(!ViewTransition(renderer).Pending(), "window resize cancels stale coordinates");
            vm.pane.scroll_y = 0;
            vm.pane.view_mode = ViewMode::LargeIcons;
            draw(6000); draw(6110, L"narrow-middle"); draw(6220, L"narrow-end");
            check(!ViewTransition(renderer).Pending(), "narrow pane transition settles");
            vm.pane.entries.clear(); draw(6300);
            check(!ViewTransition(renderer).Pending() && ViewTransition(renderer).CachedIcons() == 0,
                "empty view drops captured icons");
            if (scale == 1.0f && !dark) {
                vm.pane.entries = fixture_entries;
                vm.pane.entries.resize(100000);
                vm.pane.view_mode = ViewMode::Details;
                vm.pane.scroll_x = vm.pane.scroll_y = 0;
                draw(7000);
                vm.pane.view_mode = ViewMode::MediumIcons;
                draw(7100); draw(7210);
                check(ViewTransition(renderer).Pending() && ViewTransition(renderer).CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
                    "100000-entry fixture retains only visible icon pictures");
                draw(7320);
            }
            vm.pane.entries = fixture_entries;
            for (size_t i = 0; i < vm.pane.entries.size(); ++i) {
                auto& entry = vm.pane.entries[i];
                entry.name = (i % 3 ? L"match folder " : L"other folder ") + std::to_wstring(i);
                entry.path = vm.pane.path + L"\\" + entry.name;
            }
            vm.pane.view_mode = ViewMode::Details;
            vm.pane.filter_map.reset(); vm.pane.filter_text.clear();
            vm.pane.scroll_x = vm.pane.scroll_y = 0;
            ++vm.pane.view_generation;
            draw(8000);
            auto filtered = std::make_shared<PaneViewModel::FilterMap>();
            for (int i = 0; i < 80; ++i) if (i % 3) filtered->push_back(i);
            vm.pane.filter_map = filtered; vm.pane.filter_text = L"match";
            ++vm.pane.view_generation;
            draw(8100); draw(8180, L"filter-reflow-middle");
            check(Surface(renderer).Pending() && (pixels_enabled ? Surface(renderer).CachedPixelRows() > 0 : Surface(renderer).CachedPixelRows() == 0),
                "filtered Details rows keep identity motion with the configured replay policy");
            draw(8340, L"filter-reflow-end");
            check(!Surface(renderer).Pending() && Surface(renderer).CachedPixelRows() == 0,
                "settled filtered rows return to live sharp text and release the pixel atlas");
        }
        renderer.SetCompositor(nullptr);
    }
    DestroyWindow(hwnd);
    std::printf("view-mode fixture: %d failures\n", failures);
    return failures ? 1 : 0;
}
