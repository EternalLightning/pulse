// SPDX-License-Identifier: Apache-2.0
#include "../ui/item_reflow_surface.h"
#include <cstdio>

namespace {
using namespace pulse::ui;
int failures = 0;
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}

unsigned Pixel(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, UINT x, UINT y) {
    ComPtr<ID2D1Bitmap1> sample;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    if (FAILED(dc->CreateBitmap({1, 1}, nullptr, 0, properties, &sample))) return 0;
    const D2D1_RECT_U region{x, y, x + 1, y + 1};
    if (FAILED(sample->CopyFromBitmap(nullptr, target, &region))) return 0;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(sample->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return 0;
    const unsigned color = (static_cast<unsigned>(mapped.bits[2]) << 16) |
        (static_cast<unsigned>(mapped.bits[1]) << 8) | mapped.bits[0];
    sample->Unmap();
    return color;
}
}

int main() {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> dc;
    const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
        SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
        SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc));
    Check(graphics, "create isolated Direct2D bitmap-cache fixture");
    if (!graphics) return 1;
    ComPtr<ID2D1Bitmap1> target;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Check(SUCCEEDED(dc->CreateBitmap({240, 120}, nullptr, 0, properties, &target)), "create fixture target");
    if (!target.get()) return 1;
    dc->SetTarget(target.get());
    ComPtr<ID2D1SolidColorBrush> brush;
    Check(SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 0, 0), &brush)), "create row brush");
    if (!brush.get()) return 1;
    const D2D1_RECT_F viewport{0, 0, 240, 120};
    std::vector<ItemReflowSlot> before{
        {L"a", {0, 0, 200, 20}, 0}, {L"b", {0, 20, 200, 40}, 1}};
    std::vector<ItemReflowSlot> changed{
        {L"new", {0, 0, 200, 20}, 0},
        {L"a", {0, 20, 200, 40}, 1}, {L"b", {0, 40, 200, 60}, 2}};
    ItemReflowSurface surface;
    int dynamic_draws = 0;
    bool asset_green = false;
    const auto frame = [&](const std::vector<ItemReflowSlot>& slots, uint64_t now, bool dynamic = false) {
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0));
        surface.Begin(slots, L"bitmap-fixture", viewport, now, true);
        for (const auto& slot : slots) {
            const bool interactive = dynamic && slot.key == L"b";
            surface.Paint(dc.get(), slot.key, slot.bounds, [&] {
                if (interactive) ++dynamic_draws;
                brush->SetColor(interactive ? D2D1::ColorF(0, 0, 1) :
                    asset_green ? D2D1::ColorF(0, 1, 0) : D2D1::ColorF(1, 0, 0));
                dc->FillRectangle(slot.bounds, brush.get());
            }, !interactive);
        }
        surface.FinishFrame(dc.get());
        Check(SUCCEEDED(dc->EndDraw()), "row cache frame completes without Direct2D errors");
    };
    frame(before, 100);
    frame(changed, 110);
    Check(surface.CachedPixelRows() == 2 && surface.PixelCacheBuilds() == 1,
        "moving stable rows share one atlas capture");
    frame(changed, 150);
    Check(surface.CachedPixelRows() == 2 && surface.PixelCacheBuilds() == 1,
        "textured movement reuses its atlas without repacking");
    Check(Pixel(dc.get(), target.get(), 100, 30) == 0xFF0000,
        "atlas replay preserves solid interior pixels at the sampled row position");
    frame(changed, 170, true);
    frame(changed, 190, true);
    Check(dynamic_draws == 2 && surface.CachedPixelRows() == 1 && surface.PixelCacheBuilds() == 1,
        "interactive rows draw live and do not rebuild the remaining atlas");
    Check(Pixel(dc.get(), target.get(), 100, 35) == 0x0000FF,
        "live interactive color replaces the former cached row pixels");
    frame(changed, 250);
    Check(surface.CachedPixelRows() == 3 && surface.PixelCacheBuilds() == 2,
        "newly revealed and formerly interactive commands join one replacement atlas");
    const auto before_refresh = surface.BackgroundVisual(L"b", changed[2].bounds, 0);
    surface.SetPaintRevision(1);
    const auto after_refresh = surface.BackgroundVisual(L"b", changed[2].bounds, 0);
    Check(surface.Pending() && surface.CachedRows() == 0 &&
        before_refresh.bounds.top == after_refresh.bounds.top &&
        before_refresh.opacity == after_refresh.opacity,
        "asset revision invalidates cached paint while preserving sampled motion");
    asset_green = true;
    frame(changed, 250);
    Check(Pixel(dc.get(), target.get(), 100, 45) == 0x00FF00 &&
        surface.PixelCacheBuilds() == 3,
        "changed asynchronous asset pixels refresh during motion");
    surface.SetPaintRevision(1);
    frame(changed, 270);
    Check(surface.CachedPixelRows() == 3 && surface.PixelCacheBuilds() == 3,
        "unchanged asset revision preserves the refreshed atlas");
    frame(changed, 350);
    Check(!surface.Pending() && surface.CachedRows() == 0 && surface.CachedPixelRows() == 0,
        "settled rows release commands and atlas references");
    ItemReflowSurface oversized;
    std::vector<ItemReflowSlot> large{{L"large", {0, 0, 5000, 1000}, 0}};
    oversized.Begin(large, L"oversized", viewport, 0, true);
    large[0].bounds.top += 20; large[0].bounds.bottom += 20;
    for (uint64_t now : {10ull, 30ull}) {
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0));
        oversized.Begin(large, L"oversized", viewport, now, true);
        oversized.Paint(dc.get(), large[0].key, large[0].bounds,
            [&] { dc->FillRectangle(large[0].bounds, brush.get()); });
        oversized.FinishFrame(dc.get());
        Check(SUCCEEDED(dc->EndDraw()), "oversized atlas falls back without a drawing error");
    }
    Check(oversized.CachedRows() == 1 && oversized.CachedPixelRows() == 0 &&
        oversized.PixelCacheBuilds() == 0, "atlas pixel budget retains command replay fallback");
    surface.Reset();
    dc->SetTarget(nullptr);
    return failures ? 1 : 0;
}
