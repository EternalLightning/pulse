#include "../app/settings_value_dialog.h"
#include "../common/localization.h"
#include "../ui/ui_compositor.h"
#include <wincodec.h>
#include <filesystem>
#include <cstdio>

namespace {
int failures = 0;
HWND owner = nullptr;
bool accept = false;
bool close_action = false;
bool dark = false;
int phase = 0;
int ticks = 0;
UINT target_dpi = 96;
std::filesystem::path output;
void Check(bool ok, const char* message) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
bool Capture(HWND window, const std::filesystem::path& path, bool from_screen = false) {
    RECT rc{};
    GetWindowRect(window, &rc);
    const int width = rc.right - rc.left, height = rc.bottom - rc.top;
    HDC screen = GetDC(from_screen ? nullptr : window);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(dc, bitmap);
    if (from_screen) DwmFlush();
    const BOOL printed = from_screen
        ? BitBlt(dc, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT)
        : PrintWindow(window, dc, 0);
    if (!from_screen && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION)) {
        const auto theme = pulse::ui::MakeTheme(dark, pulse::ui::HexColor(0x0078D4));
        const auto expected = [](D2D1_COLOR_F c) {
            return RGB(static_cast<BYTE>(c.r * 255), static_cast<BYTE>(c.g * 255), static_cast<BYTE>(c.b * 255));
        };
        RECT client{};
        GetClientRect(window, &client);
        const auto px = [&](int dip) { return MulDiv(dip, client.right, 360); };
        Check(GetPixel(dc, px(4), px(4)) == expected(theme.surface_title),
            "PrintWindow caption pixels match the actual light/dark title token");
        HWND edit = FindWindowExW(window, nullptr, L"EDIT", nullptr);
        RECT edit_rect{};
        GetWindowRect(edit, &edit_rect);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&edit_rect), 2);
        Check(GetPixel(dc, edit_rect.right - px(5), edit_rect.top + px(5)) == expected(theme.fill_input_focus),
            "PrintWindow input pixels match the actual light/dark input token");
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(from_screen ? nullptr : window, screen);
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
    if (++ticks > 40) {
        Check(false, "dialog keyboard automation timed out");
        if (dialog) PostMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    if (!dialog || !IsWindowVisible(dialog)) return;
    HWND edit = FindWindowExW(dialog, nullptr, L"EDIT", nullptr);
    if (phase == 0) {
        if (target_dpi != 96 && !pulse::ui::IsHighContrast()) {
            RECT resized{}; GetWindowRect(dialog, &resized);
            resized.right = resized.left + MulDiv(360, static_cast<int>(target_dpi), 96);
            resized.bottom = resized.top + MulDiv(180, static_cast<int>(target_dpi), 96);
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
        if (!pulse::ui::IsHighContrast()) {
            Check(!(GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CAPTION) &&
                !(GetWindowLongPtrW(GetDlgItem(dialog, 101), GWL_STYLE) & WS_TABSTOP),
                "custom themed caption replaces native chrome and close is excluded from Tab order");
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
        Check(GetFocus() == edit, "input initially owns focus");
        Check(HIWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == DC_HASDEFID &&
            LOWORD(SendMessageW(dialog, DM_GETDEFID, 0, 0)) == IDOK, "Enter defaults to Apply");
        Check(Capture(dialog, output / (dark ? L"settings-value-dark.png" : L"settings-value-light.png")),
            "capture native settings prompt with PrintWindow and WIC");
        const bool screen_capture = Capture(dialog, output / (dark ? L"settings-value-dark-screen.png" : L"settings-value-light-screen.png"), true);
        printf("[INFO] composed desktop prompt capture: %s\n", screen_capture ? "saved" : "unavailable");
        SetWindowTextW(edit, L"42");
        PostMessageW(edit, WM_KEYDOWN, VK_TAB, 0);
        phase = 1;
    } else if (phase == 1) {
        Check(GetFocus() == GetDlgItem(dialog, IDOK), "Tab moves from input to Apply");
        SetFocus(edit);
        if (close_action) PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(101, BN_CLICKED), 0);
        else PostMessageW(edit, WM_KEYDOWN, accept ? VK_RETURN : VK_ESCAPE, 0);
        phase = 2;
    }
}
}
int wmain() {
    OleInitialize(nullptr);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    output = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / L"review-changes" / L"feedback-ui";
    std::error_code error;
    std::filesystem::create_directories(output, error);
    Check(!error, "create isolated E-drive output directory");
    owner = CreateWindowExW(0, L"STATIC", L"Settings fixture owner", WS_OVERLAPPEDWINDOW,
        60, 60, 800, 550, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(owner, SW_SHOW);
    for (int scenario : {0, 1, 2}) {
        const bool mode = scenario != 0;
        dark = mode;
        accept = scenario == 0;
        close_action = scenario == 2;
        target_dpi = scenario == 0 ? 96u : scenario == 1 ? 144u : 192u;
        phase = 0;
        ticks = 0;
        std::wstring result = L"unchanged";
        const UINT_PTR timer = SetTimer(nullptr, 0, 250, Drive);
        const bool accepted = pulse::app::PromptSettingsValue(owner,
            mode ? L"主题色" : L"壁纸可见度", L"输入设置值", mode ? L"#0078D4" : L"85",
            result, 32, {}, mode);
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
