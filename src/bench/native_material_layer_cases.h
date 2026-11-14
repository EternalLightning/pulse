// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once

int RunNativeMaterialLayerCases() {
    using namespace pulse;
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const HWND window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"STATIC",
        L"Native material layer fixture", WS_POPUP, -20000, -20000, 1100, 760,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor;
    if (!window || !compositor.Init(window) || !compositor.UsesTransparentComposition()) return 2;
    ui::MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    renderer.SetDetailsPanelVisible(false);
    renderer.SetScale(1);
    compositor.RecreateTextFormats(1);
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    for (bool dark : {false, true}) for (auto effect :
        {ui::WindowEffect::Acrylic, ui::WindowEffect::Mica, ui::WindowEffect::MicaAlt}) {
        for (int state = 0; state < 3; ++state) {
            ui::WindowViewModel vm;
            vm.dark = dark;
            vm.window_effect = effect;
            vm.backdrop_enabled = state != 2;
            vm.pane.path = L"C:\\isolated-material-fixture";
            ui::TabView tab;
            tab.title = L"Fixture";
            tab.active = true;
            vm.tabs = {tab};
            if (state == 1) vm.background_image = L"Z:\\missing-material-fixture\\absent.png";
            auto* dc = compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            renderer.Render(vm, D2D1::RectF(0, 0, 1100, 760), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(dc->EndDraw()), "material layer frame renders");
            ui::ComPtr<ID2D1Image> image;
            ui::ComPtr<ID2D1Bitmap1> target, readable;
            dc->GetTarget(&image);
            bool copied = image.get() && SUCCEEDED(image->QueryInterface(IID_PPV_ARGS(&target)));
            if (copied) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
                copied = SUCCEEDED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, props, &readable)) &&
                    SUCCEEDED(readable->CopyFromBitmap(nullptr, target.get(), nullptr));
            }
            D2D1_MAPPED_RECT mapped{};
            const bool mapped_ok = copied && SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            check(mapped_ok, "read actual renderer pixels including alpha");
            if (!mapped_ok) continue;
            const auto alpha = [&](int x, int y) { return mapped.bits[y * mapped.pitch + x * 4 + 3]; };
            if (state == 0) {
                check(alpha(500, 15) <= 1, "native material has no duplicate full-window title tint");
                const bool mica = effect != ui::WindowEffect::Acrylic;
                const bool alt = effect == ui::WindowEffect::MicaAlt;
                const int sheet = mica ? (dark ? (alt ? 115 : 76) : (alt ? 179 : 128)) : (dark ? 140 : 153);
                check(std::abs(alpha(100, 400) - sheet) <= 1,
                    "native reading layer uses the material-specific WinUI opacity");
                if (mica) {
                    check(std::abs(alpha(80, 40) - alpha(100, 400)) <= 1,
                        "active tab uses the same material reading layer as its connected toolbar");
                    const auto* sheet_pixel = mapped.bits + 400 * mapped.pitch + 100 * 4;
                    const int color = dark ? 58 : 255;
                    check(std::abs(sheet_pixel[2] - color * sheet / 255.0f) <= 1,
                        "Mica reading layer uses the WinUI color instead of the opaque theme canvas");
                    const int pane = alt ? (dark ? 76 : 128) : 0;
                    const float pane_alpha = alt ? pane / 255.0f : 0.045f;
                    const float expected_alpha = sheet + pane_alpha * (255 - sheet);
                    check(std::abs(alpha(1000, 600) - expected_alpha) <= 2,
                        "Mica keeps a thin pane plate and Mica Alt adds the WinUI content layer");
                }
                check(alpha(1000, 600) > alpha(100, 400) && alpha(1000, 600) < 255,
                    "native material retains the translucent file-pane reading layer");
            } else if (state == 1) {
                check(alpha(500, 15) >= 180, "pending or unavailable custom image retains its existing cover");
            } else {
                check(alpha(500, 15) == 255, "unavailable native material retains an opaque theme background");
            }
            readable->Unmap();
        }
    }
    compositor.Shutdown();
    DestroyWindow(window);
    OleUninitialize();
    std::cout << "[SUMMARY] " << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
