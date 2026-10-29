// ui_compositor.h — D3D11 + D2D + DWrite device, Mica, screenshot.
#pragma once
#include "FluentTokens.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <memory>
#include <string>
#include <string_view>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace pulse::ui {

class LumaTextRenderer;
class EditComposition;
struct LumaTextStats;

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept { p = o.p; o.p = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept { if (p) p->Release(); p = o.p; o.p = nullptr; return *this; }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
    T* get() const { return p; }
    void reset() { if (p) p->Release(); p = nullptr; }
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool Init(HWND hwnd);
    const std::wstring& InitializationError() const { return initialization_error_; }
    void Shutdown();
    void Resize(int width, int height);
    void Present();
    // A hidden window has no DWM open transition. Fade the composed visual in
    // on its next presented frame instead of popping into view.
    void QueueShowTransition();
    bool CanScheduleAnimationFrame() const {
        return !animation_frames_paused_ && !device_lost_ && swapChain_.get() &&
               targetBitmap_.get() && width_ > 0 && height_ > 0;
    }
    void NotifyDeviceLost(HRESULT reason);
    bool Recover();
    bool NeedsRecovery() const { return device_lost_; }
    bool SaveSnapshot(const wchar_t* path);

    ID2D1DeviceContext* Dc() const { return dc_.get(); }
    // Menus sample the rendered frame without a desktop capture or per-frame
    // copy. Capture only when a menu is about to open, not after every Present.
    bool CaptureFrame();
    ID2D1Bitmap1* FrameCopy() const { return frame_copy_.get(); }
    IDWriteFactory2* DwriteFactory() const { return dwriteFactory_.get(); }
    HWND Hwnd() const { return hwnd_; }

    void RecreateTextFormats(float scale);
    bool UpdateTextRenderingParams(HMONITOR monitor);
    IDWriteTextFormat* TextFormat() const { return textFormat_.get(); }
    IDWriteTextFormat* FileNameFormat() const { return fileNameFormat_.get(); }
    IDWriteTextFormat* SmallFormat() const { return smallFormat_.get(); }
    IDWriteTextFormat* HeaderFormat() const { return headerFormat_.get(); }
    IDWriteTextFormat* TabFormat() const { return tabFormat_.get(); }
    IDWriteTextFormat* AddressFormat() const { return addressFormat_.get(); }
    IDWriteTextFormat* IconFormat() const { return iconFormat_.get(); }
    bool DrawLumaText(std::wstring_view text, IDWriteTextFormat* format,
                      const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                      const D2D1_COLOR_F& background,
                      DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    void DrawTextLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                        ID2D1SolidColorBrush* brush,
                        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);
    bool DrawLumaTextSplit(std::wstring_view text, IDWriteTextFormat* format,
                           const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                           std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                           const D2D1_COLOR_F& background);
    bool MeasureLumaText(std::wstring_view text, IDWriteTextFormat* format,
                         float& width, float* height = nullptr);
    bool PaintLumaEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                       const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background);
    bool PresentLumaEdit(HWND hwnd, IDWriteTextFormat* format,
                         const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background);
    LRESULT CallLumaEditMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                              IDWriteTextFormat* format);
    bool LumaTextEnabled() const noexcept;
    bool LumaTextAvailable() const noexcept;
    bool CustomEditEnabled() const noexcept;
    bool UsesComposedEdits(HWND edit) const;
    void SynchronizeEditSurface(HWND edit);
    void SetEditOpacity(HWND edit, float opacity);
    void RemoveEditSurface(HWND edit);
    void SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    const LumaTextStats* GetLumaTextStats() const noexcept;

    int Width() const { return width_; }
    int Height() const { return height_; }
    bool UsesTransparentComposition() const { return transparentComposition_; }

private:
    bool CheckGraphics(HRESULT hr, const wchar_t* stage);
    std::wstring initialization_error_;
    bool InitD3D();
    bool CreateSwapChain();
    void ResizeSwapChain();
    static bool IsDeviceLost(HRESULT hr);

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<IDXGIDevice1> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDCompositionDevice> compositionDevice_;
    ComPtr<IDCompositionTarget> compositionTarget_;
    ComPtr<IDCompositionVisual> compositionVisual_;
    ComPtr<IDCompositionEffectGroup> opacityEffect_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> targetBitmap_;
    ComPtr<ID2D1Bitmap1> frame_copy_;
    ComPtr<IDWriteFactory2> dwriteFactory_;
    ComPtr<IDWriteRenderingParams2> textRenderingParams_;
    ComPtr<IDWriteTextFormat> textFormat_;
    ComPtr<IDWriteTextFormat> fileNameFormat_;
    ComPtr<IDWriteTextFormat> smallFormat_;
    ComPtr<IDWriteTextFormat> headerFormat_;
    ComPtr<IDWriteTextFormat> tabFormat_;
    ComPtr<IDWriteTextFormat> addressFormat_;
    ComPtr<IDWriteTextFormat> iconFormat_;
    std::unique_ptr<LumaTextRenderer> lumaText_;
    std::unique_ptr<EditComposition> editComposition_;
    HMONITOR text_params_monitor_ = nullptr;
    int text_params_mode_ = -1;
    bool transparentComposition_ = false;
    bool device_lost_ = false;
    bool animation_frames_paused_ = true;
    bool show_transition_pending_ = false;
};

} // namespace pulse::ui
