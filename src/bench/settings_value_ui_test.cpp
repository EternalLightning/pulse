// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/settings_value_dialog.h"
#include "../common/localization.h"
#include "../ui/ui_compositor.h"
#include "../ui/input_dialog.h"
#include "../ui/typography.h"
#include <cmath>
#include <functional>
#include <algorithm>
#include <commctrl.h>
#include <wincodec.h>
#include <filesystem>
#include <cstdio>
#include <vector>
#if defined(PULSE_HAS_LUMATEXT)
#include <lumatext/lumatext.hpp>
#endif

namespace {
int failures = 0;
HWND owner = nullptr;
bool accept = false;
bool close_action = false;
bool dark = false;
D2D1_COLOR_F accent = pulse::ui::HexColor(0x0078D4);
int phase = 0;
int ticks = 0;
UINT target_dpi = 96;
std::filesystem::path output;
void Check(bool ok, const char* message) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
pulse::ui::InputDialogInspection Inspect(HWND dialog, bool render = false) {
    pulse::ui::InputDialogInspection info;
    Check(SendMessageW(dialog, pulse::ui::kInputDialogInspectMessage, render,
        reinterpret_cast<LPARAM>(&info)) != 0, "inspect the rendered input dialog");
    return info;
}
constexpr UINT kUiTestCall = WM_APP + 0x394;
LRESULT CALLBACK UiTestProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR) {
    if (message == kUiTestCall) {
        (*reinterpret_cast<std::function<void()>*>(lp))();
        return 1;
    }
    return DefSubclassProc(window, message, wp, lp);
}
void OnUi(HWND dialog, std::function<void()> action) {
    SendMessageW(GetWindow(dialog, GW_OWNER), kUiTestCall, 0, reinterpret_cast<LPARAM>(&action));
}
bool ReadPixels(HWND dialog, const std::function<bool(D2D1_SIZE_U, const D2D1_MAPPED_RECT&)>& read) {
    bool result = false;
    OnUi(dialog, [&] {
        const auto info = Inspect(dialog, true);
        if (!info.compositor || !info.compositor->CaptureFrame()) return;
        auto* frame = info.compositor->FrameCopy();
        if (!frame) return;
        const auto size = frame->GetPixelSize();
        pulse::ui::ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, frame->GetPixelFormat());
        if (FAILED(info.compositor->Dc()->CreateBitmap(size, nullptr, 0, &properties, &bitmap)) ||
            FAILED(bitmap->CopyFromBitmap(nullptr, frame, nullptr))) return;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(bitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return;
        result = read(size, mapped);
        bitmap->Unmap();
    });
    return result;
}
bool Pixel(HWND dialog, float x, float y, D2D1_COLOR_F expected) {
    return ReadPixels(dialog, [&](D2D1_SIZE_U size, const D2D1_MAPPED_RECT& mapped) {
        const UINT px = static_cast<UINT>(x), py = static_cast<UINT>(y);
        if (px >= size.width || py >= size.height) return false;
        const auto* pixel = mapped.bits + py * mapped.pitch + px * 4;
        return std::abs(pixel[2] - std::lround(expected.r * 255)) <= 1 &&
            std::abs(pixel[1] - std::lround(expected.g * 255)) <= 1 &&
            std::abs(pixel[0] - std::lround(expected.b * 255)) <= 1;
    });
}
bool TitleMatchesReference(HWND dialog) {
    const auto info = Inspect(dialog);
    if (!info.compositor) return false;
    const int left = static_cast<int>(std::lround(48 * info.scale));
    const int width = static_cast<int>(std::lround(info.close.left - 8 * info.scale)) - left;
    const int height = static_cast<int>(std::lround(info.close.bottom));
    wchar_t text[512]{}; GetWindowTextW(dialog, text, ARRAYSIZE(text));
    const auto theme = info.high_contrast ? pulse::ui::MakeHighContrastTheme() : pulse::ui::MakeTheme(dark, accent);
    std::vector<BYTE> expected;
    OnUi(dialog, [&] {
        using namespace pulse::ui;
        auto& compositor = *info.compositor;
        auto* dc = compositor.Dc();
        ComPtr<ID2D1Image> previous;
        dc->GetTarget(&previous);
        ComPtr<ID2D1Bitmap1> target, staging;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &target))) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(typography::CreateTextLayout(compositor.DwriteFactory(), text,
            static_cast<UINT32>(wcslen(text)), compositor.DialogTitleFormat(),
            static_cast<float>(width), static_cast<float>(height), &layout))) return;
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TEXT_METRICS metrics{}; DWRITE_LINE_METRICS line{}; UINT32 count = 0;
        if (FAILED(layout->GetMetrics(&metrics)) || FAILED(layout->GetLineMetrics(&line, 1, &count))) return;
        const float baseline = metrics.top + line.baseline;
        const auto origin = D2D1::Point2F(0, std::round(baseline) - baseline);
        dc->SetTarget(target.get()); dc->BeginDraw(); dc->Clear(theme.surface_title);
        bool rendered = false;
#if defined(PULSE_HAS_LUMATEXT)
        if (compositor.LumaTextEnabled()) {
            auto context_desc = LumaText::Descriptor<lt_context_desc>();
            context_desc.dwrite_factory = compositor.DwriteFactory();
            LumaText::Context context; LumaText::Renderer renderer; LumaText::Frame frame;
            auto renderer_desc = LumaText::Descriptor<lt_d2d_desc>();
            renderer_desc.render_target = dc; renderer_desc.manage_begin_end_draw = false;
            auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
            frame_desc.dpi_x = frame_desc.dpi_y = 96;
            if (lt_context_create(&context_desc, context.put()) == LT_OK &&
                lt_d2d_renderer_create(context.get(), &renderer_desc, renderer.put()) == LT_OK &&
                lt_frame_begin(renderer.get(), &frame_desc, frame.put()) == LT_OK) {
                auto draw = LumaText::Descriptor<lt_draw_text_desc>();
                draw.origin_y = origin.y;
                draw.foreground = {theme.text.r, theme.text.g, theme.text.b, theme.text.a};
                draw.background_type = LT_BACKGROUND_TRANSPARENT;
                draw.render_config = LumaText::Descriptor<lt_render_config>();
                draw.render_config.coverage_gamma = draw.render_config.coverage_contrast = 1;
                draw.render_config.raster_filter = LT_RASTER_FILTER_DIRECT;
                draw.render_config.flags = LT_RENDER_CONFIG_DISABLE_STEM_COMPENSATION |
                    LT_RENDER_CONFIG_AUTOHINT_LIGHT | LT_RENDER_CONFIG_DISABLE_OPTICAL_GAMMA;
                const auto result = lt_frame_draw_layout(frame.get(), layout.get(), &draw);
                rendered = lt_frame_end(frame.get()) == LT_OK && result == LT_OK;
            }
        } else
#endif
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(dc->CreateSolidColorBrush(theme.text, &brush))) {
                dc->DrawTextLayout(origin, layout.get(), brush.get()); rendered = true;
            }
        }
        const HRESULT ended = dc->EndDraw(); dc->SetTarget(previous.get());
        auto read_props = props;
        read_props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        if (!rendered || FAILED(ended) || FAILED(dc->CreateBitmap(D2D1::SizeU(width, height),
            nullptr, 0, &read_props, &staging)) || FAILED(staging->CopyFromBitmap(nullptr, target.get(), nullptr))) return;
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return;
        expected.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
            std::copy_n(mapped.bits + y * mapped.pitch, width * 4, expected.data() + y * width * 4);
        staging->Unmap();
    });
    if (expected.empty()) return false;
    return ReadPixels(dialog, [&](D2D1_SIZE_U size, const D2D1_MAPPED_RECT& mapped) {
        if (left + width > static_cast<int>(size.width) || height > static_cast<int>(size.height)) return false;
        size_t different = 0, ink = 0;
        for (int y = 0; y < height - 1; ++y) for (int x = 0; x < width; ++x) {
            const auto* reference = expected.data() + (y * width + x) * 4;
            const auto* actual = mapped.bits + y * mapped.pitch + (left + x) * 4;
            bool mismatch = false;
            for (int channel = 0; channel < 3; ++channel) mismatch |= std::abs(actual[channel] - reference[channel]) > 1;
            different += mismatch;
            ink += reference[0] != expected[0] || reference[1] != expected[1] || reference[2] != expected[2];
        }
        printf("[INFO] selected title reference: dpi=%u high_contrast=%d different_pixels=%zu ink_pixels=%zu\n",
            target_dpi, info.high_contrast, different, ink);
        return ink > 0 && different == 0;
    });
}
bool CloseGlyph(HWND dialog) {
    const auto info = Inspect(dialog);
    return ReadPixels(dialog, [&](D2D1_SIZE_U size, const D2D1_MAPPED_RECT& mapped) {
        int left = static_cast<int>(size.width), top = static_cast<int>(size.height), right = -1, bottom = -1;
        const auto* background = mapped.bits + (static_cast<int>(info.close.top) + 3) * mapped.pitch +
            (static_cast<int>(info.close.left) + 3) * 4;
        for (int y = static_cast<int>(info.close.top) + 3; y < info.close.bottom - 3; ++y)
            for (int x = static_cast<int>(info.close.left) + 3; x < info.close.right - 3; ++x) {
                const auto* pixel = mapped.bits + y * mapped.pitch + x * 4;
                if (std::abs(pixel[0] - background[0]) + std::abs(pixel[1] - background[1]) +
                    std::abs(pixel[2] - background[2]) < 150) continue;
                left = (std::min)(left, x); right = (std::max)(right, x);
                top = (std::min)(top, y); bottom = (std::max)(bottom, y);
            }
        return right >= left && right - left >= 8 * info.scale && right - left <= 16 * info.scale &&
            bottom - top >= 8 * info.scale && bottom - top <= 16 * info.scale &&
            std::abs((left + right + 1) - (info.close.left + info.close.right)) <= 3 * info.scale &&
            std::abs((top + bottom + 1) - (info.close.top + info.close.bottom)) <= 3 * info.scale;
    });
}
bool Capture(HWND window, const std::filesystem::path& path, bool from_screen = false) {
    if (!from_screen) {
        const auto info = Inspect(window);
        const auto theme = info.high_contrast ? pulse::ui::MakeHighContrastTheme() : pulse::ui::MakeTheme(dark, accent);
        const auto sheet = pulse::ui::BlendOver(theme.surface_sheet, theme.bg);
        const auto input = pulse::ui::BlendOver(theme.fill_input_focus, sheet);
        const auto outline = info.high_contrast ? theme.stroke_card : dark
            ? pulse::ui::HexColor(0xFFFFFF, 0.08f) : pulse::ui::HexColor(0x000000, 13.0f / 255.0f);
        const auto field = info.fields.front().bounds;
        Check(Pixel(window, 4 * info.scale, 4 * info.scale, theme.surface_title),
            "rendered caption pixels match the active title token");
        Check(Pixel(window, field.right - 5 * info.scale, field.top + 5 * info.scale, input),
            "rendered input pixels match the active input token");
        Check(Pixel(window, field.left, field.top + 8 * info.scale, pulse::ui::BlendOver(outline, input)),
            "rendered input outline composites its stroke over the input fill");
        Check(Pixel(window, field.left + 30 * info.scale, field.bottom - 1,
            GetFocus() == info.fields.front().edit ? pulse::ui::BlendOver(theme.accent, input) :
                pulse::ui::BlendOver(theme.stroke_input_bottom, pulse::ui::BlendOver(outline, input))),
            "settings input underline follows native editor focus");
        const auto absolute = std::filesystem::absolute(path);
        return SendMessageW(window, pulse::ui::kInputDialogSnapshotMessage, 0,
            reinterpret_cast<LPARAM>(absolute.c_str())) != 0;
    }
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT rc{};
    GetWindowRect(window, &rc);
    const POINT center{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    const HWND covering = WindowFromPoint(center);
    if (covering != window && !IsChild(window, covering)) {
        printf("[LIMIT] desktop prompt capture is covered: dialog=%p covering=%p foreground=%p\n",
            static_cast<void*>(window), static_cast<void*>(covering), static_cast<void*>(GetForegroundWindow()));
        return false;
    }
    const int width = rc.right - rc.left, height = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(dc, bitmap);
    DwmFlush();
    const BOOL printed = BitBlt(dc, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT);
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    using pulse::ui::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> image;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    bool ok = printed && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image)) &&
        SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WriteSource(image.get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());
    DeleteObject(bitmap);
    return ok;
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = FindWindowW(L"PulseSettingsValueDialog", nullptr);
    if (dialog && (GetWindowThreadProcessId(dialog, nullptr) != GetCurrentThreadId() ||
        GetWindow(dialog, GW_OWNER) != owner)) dialog = nullptr;
    if (++ticks > 40) {
        Check(false, "dialog keyboard automation timed out");
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    if (!dialog || !IsWindowVisible(dialog)) return;
    if (phase == 0 && GetPropW(dialog, L"Pulse.DialogFadeState")) return;
    HWND edit = FindWindowExW(dialog, nullptr, L"EDIT", nullptr);
    if (phase == 0) {
        RECT initial_bounds{}; GetWindowRect(dialog, &initial_bounds);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        Check(initial_bounds.left >= monitor.rcWork.left && initial_bounds.top >= monitor.rcWork.top &&
            initial_bounds.right <= monitor.rcWork.right && initial_bounds.bottom <= monitor.rcWork.bottom,
            "settings prompt clamps owner-centered bounds into the monitor work area");
        if (target_dpi != 96) {
            RECT resized{}; GetWindowRect(dialog, &resized);
            resized.right = resized.left + MulDiv(360, static_cast<int>(target_dpi), 96);
            resized.bottom = resized.top + MulDiv(212, static_cast<int>(target_dpi), 96);
            SendMessageW(dialog, WM_DPICHANGED, MAKELONG(target_dpi, target_dpi), reinterpret_cast<LPARAM>(&resized));
            Check(GetWindowTextLengthW(edit) != 0,
                "DPI change preserves input while updating caption and controls");
        }
        Check(!IsWindowEnabled(owner), "modal owner is disabled");
        const HWND dim = reinterpret_cast<HWND>(GetPropW(owner, L"Pulse.OwnerDimOverlay"));
        BYTE alpha = 0; COLORREF key = 0; DWORD flags = 0;
        Check(IsWindow(dim) && IsWindowVisible(dim) &&
            GetLayeredWindowAttributes(dim, &key, &alpha, &flags) && alpha == 64,
            "main client area has a nonactivating dim overlay at its final opacity");
        {
            Check(!(GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CAPTION) &&
                !GetDlgItem(dialog, IDOK) && !GetDlgItem(dialog, IDCANCEL),
                "shared Direct2D caption and actions replace native chrome and buttons");
            POINT title_point{8, 8};
            ClientToScreen(dialog, &title_point);
            Check(SendMessageW(dialog, WM_NCHITTEST, 0, MAKELPARAM(title_point.x, title_point.y)) == HTCAPTION,
                "custom title retains window dragging hit test");
        }
        RECT client{};
        GetClientRect(dialog, &client);
        bool contained = true;
        for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rect), 2);
            contained = contained && rect.left >= 0 && rect.top >= 0 &&
                rect.right <= client.right && rect.bottom <= client.bottom;
        }
        Check(contained, "caption and content controls stay inside the client bounds");
        const auto px = [&](int dip) { return MulDiv(dip, client.right, 360); };
        const auto info = Inspect(dialog);
        const auto primary = info.primary, cancel = info.cancel;
        Check(info.compositor && info.compositor->CustomEditEnabled() &&
            info.fields.size() == 1 && info.fields.front().custom &&
            !GetPropW(edit, L"Pulse.NativeEditFallback"),
            "settings editor retains custom rendering after layout in its hidden host");
        LOGFONTW body_font{};
        Check(GetObjectW(reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)), sizeof(body_font), &body_font) &&
            std::wcscmp(body_font.lfFaceName, pulse::ui::typography::PreferredTextFamily()) == 0,
            "settings input and actions share the application's text font family");
        Check(primary.bottom - primary.top == px(32) && primary.left - cancel.right == px(8) &&
            client.right - primary.right == px(20) && client.bottom - primary.bottom == px(16),
            "settings actions use 32 DIP height, right-aligned Cancel then Primary and 64 DIP footer");
        {
            const auto close = info.close;
            Check(close.right - close.left == 46 * info.scale && close.bottom == 44 * info.scale,
                "settings close target follows the 46 by 44 DIP modal header");
            Check(CloseGlyph(dialog), "settings vector Close is visible at its shared size and centered in its hit target");
            Check(info.title_font_size == px(14) && info.title_weight == DWRITE_FONT_WEIGHT_BOLD,
                "settings caption uses the shared 14 DIP bold DirectWrite format");
            Check(TitleMatchesReference(dialog), "caption glyph pixels match the selected LumaText title without extra weight");
        }
        Check(GetFocus() == edit, "input initially owns focus");
        wchar_t retained[128]{}; GetWindowTextW(edit, retained, ARRAYSIZE(retained));
        {
            pulse::ui::HighContrastFrameScope contrast(true);
            SendMessageW(dialog, WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
            HDC dc = GetDC(edit);
            const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(dialog, WM_CTLCOLOREDIT,
                reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(edit)));
            LOGBRUSH brush_info{}; GetObjectW(brush, sizeof(brush_info), &brush_info);
            Check(brush_info.lbColor == GetSysColor(COLOR_WINDOW) && GetTextColor(dc) == GetSysColor(COLOR_WINDOWTEXT),
                "settings prompt refreshes input to system high-contrast palette while retaining its caption style");
            ReleaseDC(edit, dc);
            Check(Inspect(dialog).high_contrast && !(GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CAPTION) && CloseGlyph(dialog),
                "high contrast keeps the shared caption and visible vector Close");
            Check(TitleMatchesReference(dialog), "high-contrast caption matches the DirectWrite fallback reference");
        }
        SendMessageW(dialog, WM_THEMECHANGED, 0, 0);
        wchar_t restored[128]{}; GetWindowTextW(edit, restored, ARRAYSIZE(restored));
        Check(wcscmp(retained, restored) == 0, "settings theme refresh retains native input text");
        Check(Inspect(dialog).enabled, "valid native input enables the default Apply action");
        const std::wstring snapshot = std::wstring(dark ? L"settings-value-dark-" : L"settings-value-light-") +
            std::to_wstring(target_dpi) + L"dpi";
        Check(Capture(dialog, output / (snapshot + L".png")),
            "capture the actual Direct2D settings prompt frame");
        const bool screen_capture = Capture(dialog, output / (snapshot + L"-screen.png"), true);
        printf("[INFO] composed desktop prompt capture: %s\n", screen_capture ? "saved" : "unavailable");
        SetWindowTextW(edit, L"42");
        SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(IsWindow(dialog) && GetWindowTextLengthW(edit) == 2,
            "synthetic IME composition Enter and Escape preserve the open settings editor");
        SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
        SendMessageW(edit, WM_KEYDOWN, VK_TAB, 0);
        Check(GetFocus() == dialog && Inspect(dialog).focus == IDCANCEL,
            "Tab moves from input to Cancel in the shared action order");
        PostMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        phase = 1;
    } else if (phase == 1) {
        Check(GetFocus() == dialog && Inspect(dialog).focus == IDOK, "next Tab moves from Cancel to Apply");
        if (!pulse::ui::IsHighContrast()) {
            RECT client{}; GetClientRect(dialog, &client);
            const auto px = [&](int dip) { return MulDiv(dip, client.right, 360); };
            const auto theme = pulse::ui::MakeTheme(dark, accent);
            const auto input = pulse::ui::BlendOver(theme.fill_input_focus, pulse::ui::BlendOver(theme.surface_sheet, theme.bg));
            const auto outline = dark ? pulse::ui::HexColor(0xFFFFFF, 0.08f) : pulse::ui::HexColor(0x000000, 13.0f / 255.0f);
            const auto bottom = pulse::ui::BlendOver(theme.stroke_input_bottom, pulse::ui::BlendOver(outline, input));
            const auto field = Inspect(dialog).fields.front().bounds;
            Check(Pixel(dialog, field.left + px(30), field.bottom - 1, bottom),
                "Tab away from settings editor replaces accent underline with neutral bottom stroke");
        }
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        Check(GetFocus() == dialog && Inspect(dialog).focus == 101, "Tab reaches the shared caption Close after Apply");
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        Check(GetFocus() == edit, "Tab wraps from caption Close to the native input");
        if (close_action) PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), 0);
        else PostMessageW(edit, WM_KEYDOWN, accept ? VK_RETURN : VK_ESCAPE, 0);
        phase = 2;
    }
}
}
int wmain(int argc, wchar_t** argv) {
    const bool style_only = argc == 2 && wcscmp(argv[1], L"--popup-style-only") == 0;
    OleInitialize(nullptr);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    output = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / L"review-changes" / L"feedback-ui";
    if (style_only) {
        wchar_t fixture_root[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", fixture_root, ARRAYSIZE(fixture_root));
        output = length > 0 && length < ARRAYSIZE(fixture_root)
            ? std::filesystem::path(fixture_root) / L"settings-value"
            : std::filesystem::current_path() / L"bench_data" / L"settings-value";
    }
    std::error_code error;
    std::filesystem::create_directories(output, error);
    Check(!error, "create isolated E-drive output directory");
    owner = CreateWindowExW(0, L"STATIC", L"Settings fixture owner", WS_OVERLAPPEDWINDOW,
        60, 60, 800, 550, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(owner, SW_SHOW);
    Check(SetWindowSubclass(owner, UiTestProc, 9, 0) != FALSE, "install UI-thread readback dispatch");
    for (int scenario : {0, 1, 3, 2}) {
        const bool mode = scenario != 0;
        dark = mode;
        accent = pulse::ui::HexColor(style_only ? 0x87CEFA : 0x0078D4);
        accept = scenario == 0;
        close_action = scenario == 2;
        target_dpi = scenario == 0 ? 96u : scenario == 1 ? 144u : scenario == 3 ? 168u : 192u;
        if (scenario == 2) {
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
            SetWindowPos(owner, nullptr, monitor.rcWork.left - 600, monitor.rcWork.top - 400, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        phase = 0;
        ticks = 0;
        std::wstring result = L"unchanged";
        const UINT_PTR timer = SetTimer(nullptr, 0, 250, Drive);
        const bool accepted = pulse::app::PromptSettingsValue(owner,
            style_only ? L"背景图显示强度" : mode ? L"主题色" : L"壁纸可见度",
            style_only ? L"输入背景图显示强度（0–100%）" : L"输入设置值",
            style_only ? L"70" : mode ? L"#0078D4" : L"85",
            result, 32, {}, mode, pulse::ui::HexColor(style_only ? 0x87CEFA : 0x0078D4));
        KillTimer(nullptr, timer);
        Check(phase == 2 && accepted == accept, "Enter accepts while Escape and caption close cancel through dialog navigation");
        Check(result == (accept ? L"42" : L"unchanged"), "cancel never changes caller value");
        Check(IsWindowEnabled(owner), "owner is reenabled after dismissal");
        Check(GetPropW(owner, L"Pulse.OwnerDimOverlay") == nullptr,
            "owner dim overlay is removed after prompt dismissal");
    }
    DestroyWindow(owner);
    OleUninitialize();
    return failures ? 1 : 0;
}
