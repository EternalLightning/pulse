// SPDX-License-Identifier: Apache-2.0
#include "../ui/ui_renderer_internal.h"
#include "../ui/toolbar_layout.h"
#include <filesystem>
#include <cstdio>

namespace {
int ColorPixels(pulse::ui::Compositor& compositor, D2D1_RECT_F bounds, D2D1_COLOR_F color) {
    using namespace pulse::ui;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image; dc->GetTarget(&image);
    ComPtr<ID2D1Bitmap1> target, readable;
    if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return -1;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) return -1;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return -1;
    const auto size = target->GetPixelSize();
    int count = 0;
    for (int y = std::max(0, static_cast<int>(std::ceil(bounds.top))); y < std::min(static_cast<int>(size.height), static_cast<int>(bounds.bottom)); ++y)
        for (int x = std::max(0, static_cast<int>(std::ceil(bounds.left))); x < std::min(static_cast<int>(size.width), static_cast<int>(bounds.right)); ++x) {
            const auto* pixel = mapped.bits+static_cast<size_t>(y)*mapped.pitch+x*4;
            if (std::abs(pixel[0]-color.b*255) < 3 && std::abs(pixel[1]-color.g*255) < 3 &&
                std::abs(pixel[2]-color.r*255) < 3) ++count;
        }
    readable->Unmap();
    return count;
}
}
int main() {
    using namespace pulse;
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        ok ? ++passed : ++failed;
    };
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(L"../bench_data/toolbar-icon-2026-11-17");
    std::filesystem::create_directories(root);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Toolbar icon fixture",
        WS_POPUP, -30000, -30000, 1200, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor; ui::MainRenderer renderer;
    check(window && compositor.Init(window), "initialize production toolbar icon renderer");
    if (!compositor.Dc()) return 2;
    renderer.SetCompositor(&compositor); renderer.SetDetailsPanelVisible(false);
    for (const bool dark : {true, false}) for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
        const float width = 1200*scale, height = 360*scale;
        renderer.SetScale(scale); compositor.Resize(static_cast<UINT>(width), static_cast<UINT>(height));
        compositor.RecreateTextFormats(scale); renderer.InvalidateTypography();
        ui::WindowViewModel vm; vm.dark = dark; vm.safe_mode = true; vm.can_group = true;
        vm.pane.loading = true; vm.pane.header_controls_opacity = 1;
        vm.toolbar_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
        renderer.SetToolbarOrder(vm.toolbar_order);
        const auto layout = ui::MakeToolbarLayout(width, scale, renderer.TitleBarHeight(), renderer.Margin(),
            renderer.NewButtonWidthPx(false), renderer.EffectiveSidebarWidth(width), 0, false, false, vm.toolbar_order);
        check(!layout.has_overflow, "wide fixture displays the customization control");
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        const auto rect = D2D1::RectF(0,0,width,height);
        auto* dc = compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0,0));
        renderer.Render(vm, rect, theme);
        check(SUCCEEDED(dc->EndDraw()), "render toolbar icons at the selected theme and DPI");
        check(ColorPixels(compositor, layout.overflow, theme.text) > 8,
            "customize icon uses the same primary foreground as sort group and filter");
        const float left = std::round((layout.overflow.left+layout.overflow.right)/2-8*scale);
        const float top = std::round((layout.overflow.top+layout.overflow.bottom)/2-8*scale);
        if (scale == 1) check(ColorPixels(compositor, {left+9,top+4,left+14,top+5}, theme.text) >= 4,
            "native-size horizontal stroke has a solid one-pixel core without fractional-row blur");
        const auto hit = renderer.HitTest(vm, rect, (layout.overflow.left+layout.overflow.right)/2,
            (layout.overflow.top+layout.overflow.bottom)/2);
        check(hit.region == ui::HitTestResult::ToolbarMore, "customization keeps the existing button hit target");
        const auto name = (dark ? L"dark-" : L"light-")+std::to_wstring(static_cast<int>(scale*100))+L".png";
        check(compositor.SaveSnapshot((root/name).c_str()), "save toolbar foreground and pixel-alignment screenshot");
    }
    renderer.SetCompositor(nullptr); compositor.Shutdown(); DestroyWindow(window); OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
