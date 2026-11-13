// SPDX-License-Identifier: Apache-2.0
#include "../ui/surface_transition.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace pulse::ui {
struct NavigationMotionUiTest {
    static bool Released(const SurfaceTransition& surface) {
        return !surface.current_.get() && !surface.previous_.get() && !surface.pending;
    }
    static bool Settled(const SurfaceTransition& surface) {
        return surface.current_.get() && !surface.previous_.get() && !surface.pending;
    }
    static bool OutgoingReleased(const SurfaceTransition& surface) {
        return surface.pending && !surface.previous_.get();
    }
};
}

namespace {
using namespace pulse::ui;
int failures = 0;
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}

std::vector<uint32_t> Pixels(ID2D1DeviceContext* dc, ID2D1Bitmap1* target) {
    ComPtr<ID2D1Bitmap1> copy;
    const auto size = target->GetPixelSize();
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, properties, &copy)) ||
        FAILED(copy->CopyFromBitmap(nullptr, target, nullptr))) return {};
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(copy->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
    std::vector<uint32_t> pixels(static_cast<size_t>(size.width) * size.height);
    for (UINT y = 0; y < size.height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * size.width,
            mapped.bits + static_cast<size_t>(y) * mapped.pitch, size.width * sizeof(uint32_t));
    copy->Unmap();
    return pixels;
}
}

int main() {
    using namespace pulse::ui;
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
    Check(graphics, "initialize isolated surface-transition graphics");
    if (!graphics) return 1;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Check(SUCCEEDED(dc->CreateBitmap({480, 240}, nullptr, 0, properties, &target)) &&
        SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 0, 0), &brush)), "create surface target and brush");
    if (!target.get() || !brush.get()) return 1;
    dc->SetTarget(target.get());
    uint64_t hash = 1469598103934665603ull;
    for (float scale : {1.0f, 1.5f, 2.0f}) for (bool dark : {false, true}) {
        SurfaceTransition surface;
        int draws = 0;
        const auto bounds = D2D1::RectF(10 * scale, 10 * scale, 220 * scale, 110 * scale);
        const auto ink = D2D1::RectF(30 * scale, 30 * scale, 160 * scale, 80 * scale);
        const auto background = D2D1::ColorF(dark ? 0.05f : 0.95f, dark ? 0.05f : 0.95f, dark ? 0.05f : 0.95f);
        const auto draw = [&] { ++draws; dc->FillRectangle(ink, brush.get()); };
        const auto frame = [&](const wchar_t* key, uint64_t now, bool ready = true,
                               bool animate = true, D2D1_POINT_2F scroll = {}) {
            dc->BeginDraw(); dc->Clear(background);
            dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            surface.Paint(dc.get(), bounds, key, ready, now, animate, 16 * scale, 200, draw,
                {}, false, SurfaceMotion::Forward, 1, 0, scroll);
            dc->PopAxisAlignedClip();
            Check(SUCCEEDED(dc->EndDraw()), "surface frame completes with the caller clip restored");
            auto pixels = Pixels(dc.get(), target.get());
            Check(!pixels.empty(), "surface frame can be read back");
            for (uint32_t pixel : pixels) hash = (hash ^ pixel) * 1099511628211ull;
            return pixels;
        };
        const auto direct = [&] {
            dc->BeginDraw(); dc->Clear(background); draw();
            Check(SUCCEEDED(dc->EndDraw()), "direct reference frame completes");
            return Pixels(dc.get(), target.get());
        };
        brush->SetColor(D2D1::ColorF(1, 0, 0));
        const auto initial = frame(L"A", 1000);
        Check(initial == direct() && NavigationMotionUiTest::Settled(surface),
            "initial surface matches direct pixels and retains only its current content");
        brush->SetColor(D2D1::ColorF(0, 1, 0));
        const int before = draws;
        const auto updated = frame(L"A", 1010);
        Check(draws == before + 1 && updated == direct() && updated != initial,
            "settled content updates remain live without replaying stale pixels");
        brush->SetColor(D2D1::ColorF(0, 0, 1));
        Check(frame(L"B", 1100) == updated && surface.pending, "navigation starts from the displayed outgoing content");
        frame(L"B", 1140); frame(L"B", 1200);
        Check(NavigationMotionUiTest::OutgoingReleased(surface),
            "invisible outgoing content is released while the incoming motion continues");
        frame(L"C", 1210); frame(L"D", 1230); frame(L"D", 1280);
        const auto settled = frame(L"D", 1500);
        Check(settled == direct() && NavigationMotionUiTest::Settled(surface),
            "rapid navigation settles to direct pixels and releases the outgoing content");
        Check(frame(L"slow", 1600, false) == settled && surface.pending, "brief loading holds the previous pixels");
        frame(L"slow", 1650); frame(L"slow", 1700);
        Check(frame(L"slow", 1710, true, true, {0, 10}) == direct() && !surface.pending,
            "scrolling cancels the outgoing surface without stale content");
        Check(frame(L"disabled", 1720, false, false) == direct() && NavigationMotionUiTest::Released(surface),
            "disabled motion draws directly and releases all retained content");
        surface.Reset();
        Check(NavigationMotionUiTest::Released(surface), "reset releases retained commands");
    }
    std::printf("[PIXEL_HASH] %016llx\n", static_cast<unsigned long long>(hash));
    std::printf("[SURFACE_DONE] failures=%d\n", failures);
    return failures ? 1 : 0;
}
