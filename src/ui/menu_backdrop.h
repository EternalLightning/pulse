#pragma once
#include "ui_compositor.h"
#include "window_material.h"
#include "../common/windows_compat.h"
#include <d2d1effects.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {

// Capture only the desktop area a popup actually needs, immediately before it
// is shown. The swap chain alone omits the DWM backdrop underneath transparent
// window pixels, so a live update keeps its existing plate and never captures
// the menu itself.
class MenuBackdropSource {
public:
    void Reset() { bitmap_.reset(); }
    ID2D1Bitmap1* Bitmap() const { return bitmap_.get(); }
    POINT Origin() const { return {bounds_.left, bounds_.top}; }
    RECT Bounds() const { return bounds_; }
    bool Covers(const RECT& rect) const {
        return bitmap_.get() && bounds_.left <= rect.left && bounds_.top <= rect.top &&
            bounds_.right >= rect.right && bounds_.bottom >= rect.bottom;
    }

    static bool Enabled() {
        if (IsHighContrast() || !compat::ModernWindows()) return false;
        DWORD transparency = 1, bytes = sizeof(transparency);
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &transparency, &bytes);
        return transparency != 0;
    }

    bool Capture(Compositor& compositor, POINT point) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        return CaptureRect(compositor, monitor.rcMonitor);
    }

    bool CaptureRect(Compositor& compositor, RECT requested) {
        Reset();
        if (!Enabled() || !compositor.Dc() || !IsWindowVisible(compositor.Hwnd())) return false;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        RECT bounds = requested;
        bounds.left = (std::max)(bounds.left, monitor.rcMonitor.left);
        bounds.top = (std::max)(bounds.top, monitor.rcMonitor.top);
        bounds.right = (std::min)(bounds.right, monitor.rcMonitor.right);
        bounds.bottom = (std::min)(bounds.bottom, monitor.rcMonitor.bottom);
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

private:
    // Screen rectangle of the baked plate. Kept separate from RECT because the
    // width/height pair is compared as a key, not as left/top/right/bottom.
    struct Key { LONG left, top, width, height; };

public:
    bool Draw(Compositor& compositor, MenuBackdropSource& source,
              const D2D1_RECT_F& card, POINT origin,
              bool dark, float scale, bool allow_capture) {
        auto* dc = compositor.Dc();
        const HWND owner = compositor.Hwnd();
        if (!dc || !IsWindowVisible(owner) || IsIconic(owner) ||
            !MenuBackdropSource::Enabled()) {
            Reset();
            return false;
        }
        const int width = static_cast<int>(std::ceil(card.right - card.left));
        const int height = static_cast<int>(std::ceil(card.bottom - card.top));
        const Key key{origin.x + static_cast<LONG>(card.left),
            origin.y + static_cast<LONG>(card.top), width, height};
        if (width <= 0 || height <= 0) return false;
        if (plate_.get() && (key_.left != key.left || key_.top != key.top ||
                key_.width != key.width || key_.height != key.height || dark_ != dark)) Reset();
        if (!plate_.get()) {
            const float blur = 16.0f * scale;
            const int margin = static_cast<int>(std::ceil(blur * 2.0f));
            // Live Shell rows can grow the popup after it is shown. Gather some
            // spare desktop now so growth does not require sampling the popup.
            const int growth_pad = static_cast<int>(std::ceil(120.0f * scale));
            const RECT card_rect{key.left, key.top, key.left + width, key.top + height};
            const RECT capture_rect{key.left - margin - growth_pad, key.top - margin - growth_pad,
                key.left + width + margin + growth_pad, key.top + height + margin + growth_pad};
            if (allow_capture && !source.Covers(card_rect))
                source.CaptureRect(compositor, capture_rect);
            POINT client{};
            ID2D1Bitmap1* frame = nullptr;
            if (source.Covers(card_rect)) {
                frame = source.Bitmap();
                client = source.Origin();
            } else {
                // The popup is already visible; the last app frame is the only
                // clean sample left. It covers menus inside the owner window.
                if (!compositor.FrameCopy()) compositor.CaptureFrame();
                frame = compositor.FrameCopy();
                if (!frame || !ClientToScreen(owner, &client)) return false;
            }
            if (!frame) return false;
            const auto frame_size = frame->GetPixelSize();
            if (key.left >= client.x + static_cast<LONG>(frame_size.width) ||
                key.top >= client.y + static_cast<LONG>(frame_size.height) ||
                key.left + width <= client.x || key.top + height <= client.y) return false;
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
        const float radius = MakeTheme(dark, {}).radius_flyout * scale;
        if (FAILED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(card, radius, radius), &clip))) return false;
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get()), nullptr);
        dc->DrawBitmap(plate_.get(), card);
        dc->PopLayer();
        return true;
    }

private:
    ComPtr<ID2D1Bitmap1> plate_;
    Key key_{};
    bool dark_ = false;
};

} // namespace pulse::ui
