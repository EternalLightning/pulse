// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdio>
#include <cstring>
#include <vector>

namespace pulse::ui {
struct TrayCardMotionTest {
    static std::vector<uint32_t> Pixels(Compositor& compositor, D2D1_RECT_U region) {
        auto* dc = compositor.Dc();
        ComPtr<ID2D1Image> image;
        ComPtr<ID2D1Bitmap1> target, sample;
        dc->GetTarget(&image);
        if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return {};
        const auto size = D2D1::SizeU(region.right - region.left, region.bottom - region.top);
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &sample)) ||
            FAILED(sample->CopyFromBitmap(nullptr, target.get(), &region))) return {};
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
        std::vector<uint32_t> pixels(static_cast<size_t>(size.width) * size.height);
        for (UINT y = 0; y < size.height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * size.width,
                mapped.bits + static_cast<size_t>(y) * mapped.pitch, size.width * sizeof(uint32_t));
        sample->Unmap();
        return pixels;
    }

    static int Run() {
        int failures = 0;
        const auto check = [&](bool ok, const char* text) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
            failures += !ok;
        };
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Tray card motion",
            WS_POPUP, -30000, -30000, 1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Compositor compositor;
        check(hwnd && compositor.Init(hwnd), "initialize isolated tray compositor");
        if (!compositor.Dc()) { if (hwnd) DestroyWindow(hwnd); return 1; }
        check(compositor.LumaTextAvailable(), "exercise the real automatic text backend");
        MainRenderer renderer;
        renderer.SetCompositor(&compositor);
        const auto output = std::filesystem::absolute(L"bench_data/tray-card-motion");
        if (!pulse::bench::StreamSnapshots()) std::filesystem::create_directories(output);
        for (const auto mode : {typography::TextRenderMode::Auto, typography::TextRenderMode::Sharp,
                                typography::TextRenderMode::Smooth})
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true})
        for (bool cut : {false, true}) for (float opacity : {1.0f, 0.5f}) {
            typography::SetTextRenderMode(mode);
            renderer.SetScale(scale);
            compositor.RecreateTextFormats(scale);
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            renderer.painter_.BeginFrame(theme, false);
            WindowViewModel vm;
            vm.dark = dark;
            TrayCardView card;
            card.name = L"报告.pdf";
            card.folder = L"C:\\Motion\\作业";
            card.missing = true;
            card.cut = cut;
            card.ghost = true;
            card.opacity = opacity;
            card.size = 1890000;
            vm.tray_deck.cards.push_back(card);
            const auto panel = D2D1::RectF(40 * scale, 160 * scale, 420 * scale, 440 * scale);
            auto* dc = compositor.Dc();
            const auto outer = D2D1::Matrix3x2F::Translation(16 * scale, 12 * scale);
            const auto draw = [&](float dx, float dy) {
                vm.tray_deck.cards.front().dx = dx;
                vm.tray_deck.cards.front().dy = dy;
                dc->BeginDraw();
                dc->SetTransform(outer);
                dc->Clear(theme.bg);
                renderer.DrawTrayDeck(vm, panel, theme);
                D2D1_MATRIX_3X2_F after{};
                dc->GetTransform(&after);
                const bool restored = std::memcmp(&after, &outer, sizeof(after)) == 0;
                const bool rendered = SUCCEEDED(dc->EndDraw());
                check(restored && rendered, "tray draw preserves the outer transform");
            };
            // Keep asynchronous Shell icons outside the comparison region.
            const D2D1_RECT_U region{static_cast<UINT>(128 * scale), static_cast<UINT>(190 * scale),
                static_cast<UINT>(440 * scale), static_cast<UINT>(300 * scale)};
            draw(0, 0);
            const auto baseline = Pixels(compositor, region);
            draw(180, -90);
            auto moved_region = region;
            moved_region.left += static_cast<UINT>(180 * scale);
            moved_region.right += static_cast<UINT>(180 * scale);
            moved_region.top -= static_cast<UINT>(90 * scale);
            moved_region.bottom -= static_cast<UINT>(90 * scale);
            const auto moved = Pixels(compositor, moved_region);
            if (baseline.empty() || baseline != moved)
                std::printf("[CASE] scale=%.1f dark=%d cut=%d opacity=%.1f\n", scale, dark, cut, opacity);
            check(!baseline.empty() && baseline == moved,
                "drag translates card frame, filename, metadata, folder and cut badge together");
            // The second draw exercises cached text surfaces at the moved pose.
            draw(180, -90);
            check(!moved.empty() && moved == Pixels(compositor, moved_region),
                "cached tray text keeps its card pose and opacity");

            ComPtr<ID2D1Image> target;
            ComPtr<ID2D1CommandList> commands;
            dc->GetTarget(&target);
            bool recorded = SUCCEEDED(dc->CreateCommandList(&commands));
            if (recorded) {
                vm.tray_deck.cards.front().dx = vm.tray_deck.cards.front().dy = 0;
                vm.tray_deck.cards.front().opacity = 1;
                dc->SetTarget(commands.get());
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
                dc->BeginDraw();
                renderer.DrawTrayDeck(vm, panel, theme);
                recorded = SUCCEEDED(dc->EndDraw());
                dc->SetTarget(target.get());
                recorded = SUCCEEDED(commands->Close()) && recorded;
            }
            check(recorded, "record a complete resting tray card as the affine reference");
            if (!recorded) continue;
            auto& posed = vm.tray_deck.cards.front();
            posed.angle = 9;
            posed.shrink = 0.91f;
            posed.appear = 0.72f;
            posed.hover = 0.6f;
            posed.opacity = opacity;
            const auto geom = TrayStackGeometry(panel, scale, vm.tray_deck.thumb_dip);
            posed.dx = 90;
            posed.dy = -38;
            const auto pose = TrayCardPoseOf(geom, posed, 0);
            dc->BeginDraw();
            dc->Clear(theme.bg);
            dc->SetTransform(pose.m * outer);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pose.opacity), nullptr);
            dc->DrawImage(commands.get());
            dc->PopLayer();
            check(SUCCEEDED(dc->EndDraw()), "render transformed whole-card reference");
            const D2D1_RECT_U affine_region{static_cast<UINT>(250 * scale), static_cast<UINT>(110 * scale),
                static_cast<UINT>(560 * scale), static_cast<UINT>(310 * scale)};
            const auto reference = Pixels(compositor, affine_region);
            const bool snapshot = mode == typography::TextRenderMode::Auto && scale == 1.5f && dark && opacity == 1;
            if (snapshot) check(pulse::bench::SaveSnapshot(compositor,
                output / (cut ? L"affine-cut-reference.png" : L"affine-reference.png")),
                "save whole-card affine reference");
            draw(90, -38);
            const auto actual = Pixels(compositor, affine_region);
            if (snapshot) check(pulse::bench::SaveSnapshot(compositor,
                output / (cut ? L"affine-cut-actual.png" : L"affine-actual.png")),
                "save actual transformed tray card");
            size_t differences = 0;
            if (actual.size() == reference.size()) for (size_t i = 0; i < actual.size(); ++i) {
                const auto a = actual[i], b = reference[i];
                bool different = false;
                for (unsigned shift : {0u, 8u, 16u, 24u})
                    different = different || std::abs(static_cast<int>((a >> shift) & 255) -
                        static_cast<int>((b >> shift) & 255)) > 2;
                differences += different;
            }
            if (differences > 8)
                std::printf("[AFFINE] scale=%.1f dark=%d cut=%d opacity=%.1f pixels=%zu\n",
                    scale, dark, cut, opacity, differences);
            check(!actual.empty() && actual.size() == reference.size() && differences <= 8,
                "rotated, scaled and fading card matches a single transformed whole-card drawing");
        }
        renderer.SetCompositor(nullptr);
        compositor.Shutdown();
        DestroyWindow(hwnd);
        return failures ? 1 : 0;
    }
};
}
