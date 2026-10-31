// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pulse::ui::view_mode_feedback {
inline uint32_t Pixel(Compositor& compositor, float x, float y) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, sample;
    auto* dc = compositor.Dc();
    dc->GetTarget(&image);
    if (!image || FAILED(image.As(&target))) return 0xFFFFFFFFu;
    const auto size = target->GetPixelSize();
    const auto px = static_cast<UINT>(std::max(0.0f, std::floor(x)));
    const auto py = static_cast<UINT>(std::max(0.0f, std::floor(y)));
    if (px >= size.width || py >= size.height) return 0xFFFFFFFFu;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    const D2D1_RECT_U region{px, py, px + 1, py + 1};
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample)) ||
        FAILED(sample->CopyFromBitmap(nullptr, target.Get(), &region))) return 0xFFFFFFFFu;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return 0xFFFFFFFFu;
    uint32_t pixel;
    std::memcpy(&pixel, mapped.bits, sizeof(pixel));
    sample->Unmap();
    return pixel & 0x00FFFFFFu;
}
}

inline int pulse::ui::ItemReflowUiTest::RunViewModeFeedback(const std::filesystem::path& output) {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    wchar_t transport[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", transport, ARRAYSIZE(transport)) != 1 || transport[0] != L'1')
        std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"View mode feedback",
        WS_POPUP, -30000, -30000, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        if (!hwnd || !compositor.Init(hwnd)) {
            check(false, "initialize ghosting regression compositor");
            if (hwnd) DestroyWindow(hwnd);
            return 1;
        }
        const auto viewport = D2D1::RectF(0, 0, 400, 300);
        const auto small_bounds = D2D1::RectF(32, 32, 48, 48);
        const auto large_bounds = D2D1::RectF(240, 160, 304, 224);
        for (bool shrinking : {false, true}) for (uint64_t hz : {144u, 240u}) {
            ViewModeTransition transition;
            const auto old_bounds = shrinking ? large_bounds : small_bounds;
            const auto next_bounds = shrinking ? small_bounds : large_bounds;
            auto mode = ViewMode::Details;
            uint64_t signature = 1;
            bool asset_changed = false;
            int recordings = 0;
            const auto frame = [&](uint64_t now) {
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f));
                transition.Paint(dc, L"one-file", mode, viewport, {}, 1, now, true, false, [&] {
                    ++recordings;
                    const auto bounds = mode == ViewMode::Details ? old_bounds : next_bounds;
                    transition.Icon(dc, L"file", bounds, 7, [&] {
                        ComPtr<ID2D1SolidColorBrush> brush;
                        dc->CreateSolidColorBrush(mode == ViewMode::Details ? D2D1::ColorF(1, 0, 0) :
                            asset_changed ? D2D1::ColorF(0, 0, 1) : D2D1::ColorF(0, 1, 0), &brush);
                        dc->FillRectangle(bounds, brush.get());
                    });
                }, 0, signature);
                const auto result = dc->EndDraw();
                if (FAILED(result)) std::printf("Direct2D HRESULT: %08lx\n", static_cast<unsigned long>(result));
                check(SUCCEEDED(result), "high-refresh sample has no Direct2D error");
            };
            frame(1000);
            mode = ViewMode::MediumIcons;
            frame(1100);
            auto previous = transition.IconBounds(L"file");
            bool captured = false;
            for (uint64_t sample = 1; 1000 * sample / hz < ViewModeTransition::kDuration; ++sample) {
                const auto elapsed = 1000 * sample / hz;
                frame(1100 + elapsed);
                const auto bounds = transition.IconBounds(L"file");
                const auto pixel = view_mode_feedback::Pixel(compositor,
                    (bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f);
                const auto expected = shrinking ? 0x00FF0000u : asset_changed ? 0x000000FFu : 0x0000FF00u;
                check(pixel == expected, "one moving file draws one opaque high-resolution picture without crossfade ghosting");
                check(transition.Pending(), "144/240Hz samples keep requesting animation frames");
                check(shrinking ? bounds.left < previous.left : bounds.left > previous.left,
                    "icon position advances at every high-refresh sample");
                const float px = (previous.left + previous.right) * 0.5f;
                const float py = (previous.top + previous.bottom) * 0.5f;
                if (px < bounds.left - 1 || px > bounds.right + 1 || py < bounds.top - 1 || py > bounds.bottom + 1)
                    check(view_mode_feedback::Pixel(compositor, px, py) == 0,
                        "pixels outside the current icon contain no previous-frame trail");
                previous = bounds;
                if (!captured && elapsed >= 110) {
                    const int before = recordings;
                    frame(1100 + elapsed);
                    check(transition.ReusedFrame() && recordings == before,
                        "unchanged scene reuses recorded drawing without re-recording text or icons");
                    ++signature;
                    if (!shrinking) asset_changed = true;
                    frame(1100 + elapsed);
                    check(!transition.ReusedFrame() && recordings == before + 1,
                        "changed visual revision invalidates the cached incoming scene immediately");
                    if (!shrinking) check(view_mode_feedback::Pixel(compositor,
                        (bounds.left + bounds.right) * 0.5f, (bounds.top + bounds.bottom) * 0.5f) == 0x000000FFu,
                        "asynchronously updated icon pixels replace the cached image in the same frame");
                    const auto name = std::to_wstring(hz) + (shrinking ? L"-shrink-middle.png" : L"-grow-middle.png");
                    check(WriteAnimationSnapshot(compositor, output / name), "save high-refresh ghosting regression sample");
                    captured = true;
                }
            }
            check(recordings < static_cast<int>(hz * ViewModeTransition::kDuration / 2000),
                "stable transition captures far fewer pictures than display samples");
            frame(1100 + ViewModeTransition::kDuration);
            check(!transition.Pending(), "high-refresh transition settles without retaining a trail");
        }
    }
    DestroyWindow(hwnd);
    std::printf("view-mode ghosting fixture: %d failures\n", failures);
    return failures ? 1 : 0;
}
