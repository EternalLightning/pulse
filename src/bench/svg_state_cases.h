// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../common/resource_ids.h"
#include "../ui/fluent_svg_cache.h"
#include <d2d1svg.h>

inline int pulse::ui::ItemReflowUiTest::RunSvgState() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"SVG state fixture",
        WS_POPUP, -30000, -30000, 160, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Compositor compositor;
    if (!window || !compositor.Init(window)) {
        check(false, "initialize SVG state fixture");
        if (window) DestroyWindow(window);
        return 1;
    }
    MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    const D2D1_COLOR_F red = D2D1::ColorF(1, 0, 0), green = D2D1::ColorF(0, 1, 0);
    for (float scale : {1.0f, 1.5f, 2.0f}) for (int resource :
        {IDR_FILES_FILTER_SVG, IDR_FILES_STATUS_CENTER_SVG, IDR_FLUENT_HOME_SVG}) {
        renderer.SetScale(scale);
        const auto frame = [&](float opacity, const D2D1_COLOR_F& color, float phase = 0.0f,
                               bool transformed = false, bool recorded = false) {
            auto* dc = compositor.Dc();
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            ComPtr<ID2D1Image> target;
            ComPtr<ID2D1CommandList> commands;
            dc->GetTarget(&target);
            if (recorded) {
                check(SUCCEEDED(dc->CreateCommandList(&commands)), "create SVG transition command list");
                if (!commands.get()) return std::vector<uint32_t>{};
                dc->SetTarget(commands.get());
            }
            const auto transform = transformed ? D2D1::Matrix3x2F::Translation(0.25f, 0.25f)
                                               : D2D1::Matrix3x2F::Identity();
            dc->SetTransform(transform);
            check(renderer.DrawFluentSvg(resource, {8 + phase, 8, 8 + phase + 32 * scale, 8 + 32 * scale},
                opacity, &color, true), "SVG artwork renders at the requested scale");
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            if (recorded) {
                dc->SetTarget(target.get());
                check(SUCCEEDED(commands->Close()), "close SVG transition command list");
                dc->DrawImage(commands.get(), D2D1::Point2F(0.25f, 0.25f));
            }
            check(SUCCEEDED(dc->EndDraw()), "SVG state frame completes without D2D errors");
            auto pixels = view_mode_title::Pixels(compositor, {0, 0, 160, 160});
            check(!pixels.empty(), "read SVG state pixels");
            auto* document = renderer.fluent_svgs_.at(resource).get();
            ComPtr<ID2D1SvgElement> root;
            document->GetRoot(&root);
            root->SetAttributeValue(L"opacity", std::clamp(opacity, 0.0f, 1.0f));
            if (resource != IDR_FLUENT_HOME_SVG) {
                ComPtr<ID2D1SvgPaint> fill;
                if (SUCCEEDED(root->GetAttributeValue(L"fill", IID_PPV_ARGS(&fill))) && fill.get())
                    fill->SetColor(&color);
            }
            document->SetViewportSize({24, 24});
            dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
            if (recorded) {
                commands.reset();
                check(SUCCEEDED(dc->CreateCommandList(&commands)), "create vector transition reference");
                if (!commands.get()) return std::vector<uint32_t>{};
                dc->SetTarget(commands.get());
            }
            dc->SetTransform(D2D1::Matrix3x2F::Scale(32 * scale / 24, 32 * scale / 24) *
                D2D1::Matrix3x2F::Translation(8 + phase, 8) * transform);
            renderer.empty_state_svg_dc_->DrawSvgDocument(document);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            if (recorded) {
                dc->SetTarget(target.get());
                check(SUCCEEDED(commands->Close()), "close vector transition reference");
                dc->DrawImage(commands.get(), D2D1::Point2F(0.25f, 0.25f));
            }
            check(SUCCEEDED(dc->EndDraw()), "direct SVG reference renders without D2D errors");
            check(pixels == view_mode_title::Pixels(compositor, {0, 0, 160, 160}),
                "cached and vector SVG paths preserve identical pixels");
            return pixels;
        };
        const auto original = frame(1, red);
        check(FluentSvgRasterCacheBytes() > 0, "opaque integer-aligned SVG artwork populates the pixel cache");
        if (resource == IDR_FILES_FILTER_SVG && scale == 1) {
            SetFluentSvgRasterCacheEnabledForTest(false);
            check(frame(1, red) == original && FluentSvgRasterCacheBytes() == 0,
                "uncached profiling control preserves pixels without retaining SVG bitmaps");
            SetFluentSvgRasterCacheEnabledForTest(true);
            check(frame(1, red) == original && FluentSvgRasterCacheBytes() > 0,
                "cached profiling control restores the measured raster path");
        }
        check(frame(1, red) == original, "unchanged SVG state preserves identical pixels");
        check(frame(0.4f, red) != original, "changed opacity updates SVG pixels");
        check(frame(1, red) == original, "restoring opacity restores the original artwork");
        const auto recolored = frame(1, green);
        if (resource != IDR_FLUENT_HOME_SVG)
            check(recolored != original, "changed foreground updates tintable SVG pixels");
        check(frame(1, red) == original, "restoring foreground restores original pixels");
        check(frame(-1, red) == frame(0, red), "negative opacity clamps to transparent");
        check(frame(2, red) == original, "opacity above one clamps to opaque");
        frame(1, red, 0.5f); frame(1, red, 0, true); frame(1, red, 0, false, true);
        if (resource == IDR_FILES_FILTER_SVG && scale == 1) {
            for (int i = 0; i < 40; ++i) frame(1, D2D1::ColorF(i / 39.0f, 0.5f, 0.5f));
            check(FluentSvgRasterCacheBytes() <= 32 * 66 * 66 * 4,
                "SVG cache remains within its bounded pixel payload");
            check(frame(1, red) == original, "bounded cache eviction preserves the restored SVG artwork");
        }
    }
    const auto retained_bytes = FluentSvgRasterCacheBytes();
    {
        MainRenderer other;
        other.SetCompositor(&compositor);
        check(FluentSvgRasterCacheBytes() == retained_bytes,
            "another renderer does not evict existing SVG pixels");
        auto* dc = compositor.Dc();
        dc->BeginDraw();
        check(other.DrawFluentSvg(IDR_FILES_FILTER_SVG, {8, 8, 40, 40}, 1, &red, true),
            "another renderer builds its independent SVG artwork");
        check(SUCCEEDED(dc->EndDraw()) && FluentSvgRasterCacheBytes() > retained_bytes,
            "both renderers retain their SVG pixels concurrently");
        other.SetCompositor(nullptr);
        check(FluentSvgRasterCacheBytes() == retained_bytes,
            "closing another renderer preserves the original renderer cache");
    }
    renderer.SetCompositor(nullptr);
    check(FluentSvgRasterCacheBytes() == 0, "renderer teardown releases all cached SVG pixels");
    compositor.Shutdown();
    DestroyWindow(window);
    std::printf("[SVG_STATE_DONE] failures=%d\n", failures);
    return failures ? 1 : 0;
}
