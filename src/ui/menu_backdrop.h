#pragma once
#include "ui_compositor.h"
#include "window_material.h"
#include "../common/windows_compat.h"
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {

// Capture before either popup is shown: the swap chain alone omits the DWM
// backdrop underneath transparent window pixels. Both popups reuse this sample
// while their rows change, so a live update never captures the menu itself.
class MenuBackdropSource {
public:
    void Reset() { bitmap_.reset(); }
    ID2D1Bitmap1* Bitmap() const { return bitmap_.get(); }
    POINT Origin() const { return {bounds_.left, bounds_.top}; }

    static bool Enabled() {
        if (IsHighContrast() || !compat::ModernWindows()) return false;
        DWORD transparency = 1, bytes = sizeof(transparency);
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &bytes);
        return transparency != 0;
    }

    bool Capture(Compositor& compositor, POINT point) {
        Reset();
        if (!Enabled() || !compositor.Dc() || !IsWindowVisible(compositor.Hwnd())) return false;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        const RECT bounds = monitor.rcMonitor;
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) return false;
        HDC screen = GetDC(nullptr);
        if (!screen) return false;
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous = memory && dib ? SelectObject(memory, dib) : nullptr;
        const bool copied = previous && previous != HGDI_ERROR &&
            BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top,
                SRCCOPY | CAPTUREBLT) != FALSE;
        if (copied) {
            GdiFlush();
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
            compositor.Dc()->CreateBitmap(D2D1::SizeU(width, height), bits,
                static_cast<UINT32>(width * 4), props, &bitmap_);
        }
        if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
        if (dib) DeleteObject(dib);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        bounds_ = bounds;
        return bitmap_.get() != nullptr;
    }

private:
    ComPtr<ID2D1Bitmap1> bitmap_;
    RECT bounds_{};
};

// Bake a menu plate from the desktop sample (or the completed GPU frame if
// capture is unavailable). Keeping the
// layered popup preserves its rounded corners, shadow, fade and hit-test space.
class MenuBackdrop {
public:
    void Reset() { plate_.reset(); }
    bool Active() const { return plate_.get() != nullptr; }

    bool Draw(Compositor& compositor, const MenuBackdropSource& source,
              const D2D1_RECT_F& card, POINT origin,
              bool dark, float scale) {
        auto* dc = compositor.Dc();
        auto* frame = source.Bitmap() ? source.Bitmap() : compositor.FrameCopy();
        const HWND owner = compositor.Hwnd();
        if (!dc || !frame || !IsWindowVisible(owner) || IsIconic(owner) ||
            !MenuBackdropSource::Enabled()) {
            Reset();
            return false;
        }
        const int width = static_cast<int>(std::ceil(card.right - card.left));
        const int height = static_cast<int>(std::ceil(card.bottom - card.top));
        const RECT key{origin.x + static_cast<LONG>(card.left),
            origin.y + static_cast<LONG>(card.top), width, height};
        if (width <= 0 || height <= 0) return false;
        if (plate_.get() && (!EqualRect(&key_, &key) || dark_ != dark)) Reset();
        if (!plate_.get()) {
            POINT client = source.Bitmap() ? source.Origin() : POINT{};
            if (!source.Bitmap() && !ClientToScreen(owner, &client)) return false;
            const auto frame_size = frame->GetPixelSize();
            if (key.left >= client.x + static_cast<LONG>(frame_size.width) ||
                key.top >= client.y + static_cast<LONG>(frame_size.height) ||
                key.left + width <= client.x || key.top + height <= client.y) return false;
            const float blur = 16.0f * scale;
            const int margin = static_cast<int>(std::ceil(blur * 2.0f));
            const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            ComPtr<ID2D1Bitmap1> sample, plate;
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(width + margin * 2, height + margin * 2),
                    nullptr, 0, props, &sample)) ||
                FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, props, &plate))) return false;
            // Clamp extends the frame's edge beyond the window before the blur,
            // so a menu crossing that edge keeps one continuous material.
            constexpr GUID border_id{0x2a2d49c0, 0x4acf, 0x43c7,
                {0x8c, 0x6a, 0x7c, 0x4a, 0x27, 0x87, 0x4d, 0x27}};
            ComPtr<ID2D1Effect> border;
            if (FAILED(dc->CreateEffect(border_id, &border))) return false;
            border->SetInput(0, frame);
            border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
            border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);
            ComPtr<ID2D1Image> previous;
            dc->GetTarget(&previous);
            dc->SetTarget(sample.get());
            dc->Clear(dark ? HexColor(0x2B2B2B) : HexColor(0xF9F9F9));
            const float left = static_cast<float>(key.left - client.x - margin);
            const float top = static_cast<float>(key.top - client.y - margin);
            dc->DrawImage(border.get(), D2D1::Point2F(),
                D2D1::RectF(left, top, left + width + 2 * margin, top + height + 2 * margin));
            dc->SetTarget(plate.get());
            dc->Clear(D2D1::ColorF(0, 0));
            const bool painted = DrawMenuAcrylicSample(dc, sample.get(),
                D2D1::RectF(static_cast<float>(margin), static_cast<float>(margin),
                    static_cast<float>(margin + width), static_cast<float>(margin + height)),
                D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), dark, blur);
            dc->SetTarget(previous.get());
            if (!painted) return false;
            plate_ = std::move(plate);
            key_ = key;
            dark_ = dark;
        }
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (FAILED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(card, MakeTheme(dark, {}).radius_flyout * scale,
                    MakeTheme(dark, {}).radius_flyout * scale), &clip))) return false;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
        dc->DrawBitmap(plate_.get(), card);
        dc->PopLayer();
        return true;
    }

private:
    ComPtr<ID2D1Bitmap1> plate_;
    RECT key_{};
    bool dark_ = false;
};

} // namespace pulse::ui
