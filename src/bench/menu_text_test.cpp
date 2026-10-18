#include "../ui/fluent_menu.h"
#include "../ui/popup_origin.h"
#include "../common/localization.h"
#include <cstdio>
#include <cmath>
#include <filesystem>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool Begin(FluentMenu& menu, std::vector<FluentMenuItem> items) {
        menu.model_.SetItems(std::move(items));
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        menu.base_x_ = 300; menu.base_y_ = 200;
        menu.popup_pt_ = {menu.base_x_, menu.base_y_};
        menu.open_ = true; menu.animating_out_ = false; menu.result_ = 0;
        menu.hover_row_ = -1; menu.hover_swatch_ = -1;
        menu.anim_start_ = std::chrono::steady_clock::now();
        if (!menu.Render()) return false;
        SetWindowPos(menu.hwnd_, menu.topmost_ ? HWND_TOPMOST : HWND_TOP, menu.base_x_, menu.base_y_,
            menu.surf_.w, menu.surf_.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        menu.Present(64, -8);
        return true;
    }
    static void Open(FluentMenu& menu) { menu.OpenSubmenu(0); }
    static void Hover(FluentMenu& menu) { menu.UpdateHover(0); }
    static void Progress(FluentMenu& menu) { menu.Present(160, -3); }
    static void Scroll(FluentMenu& menu, float offset) {
        menu.scroll_y_ = offset;
        menu.PositionSubmenu();
    }
    static void PendingResult(FluentMenu& menu, int command) { menu.result_ = command; }
    static int Result(const FluentMenu& menu) { return menu.result_; }
    static void ChooseRow(FluentMenu& menu, int row) {
        const int margin = FluentMenu::kShadowMargin;
        POINT point{margin + 40,
            static_cast<LONG>(margin + menu.FilterHeaderPx() +
                menu.model_.RowTopPx(row) + menu.model_.RowHeightPx() * 0.5f)};
        SendMessageW(menu.hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    }
    static void ChooseSubRow(FluentMenu& menu, int row) {
        const int margin = FluentMenu::kShadowMargin;
        POINT point{margin + 40,
            static_cast<LONG>(margin + menu.sub_model_.RowTopPx(row) +
                menu.sub_model_.RowHeightPx() * 0.5f)};
        SendMessageW(menu.sub_hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    }
    static BYTE Alpha(const FluentMenu& menu) { return menu.present_alpha_; }
    static void FullyVisible(FluentMenu& menu) { menu.Present(255, 0); }
    static bool Acrylic(const FluentMenu& menu, bool submenu = false) {
        const auto& surface = submenu ? menu.sub_surf_ : menu.surf_;
        return surface.backdrop.Active();
    }
    static bool DesktopSample(const FluentMenu& menu) { return menu.backdrop_source_.Bitmap() != nullptr; }
    static bool CroppedSample(const FluentMenu& menu, float scale) {
        const RECT bounds = menu.backdrop_source_.Bounds();
        const int margin = static_cast<int>(std::ceil(16.0f * scale * 2.0f));
        const int growth_pad = static_cast<int>(std::ceil(120.0f * scale));
        return bounds.right - bounds.left <= menu.surf_.w + (margin + growth_pad) * 2 + 2 &&
            bounds.bottom - bounds.top <= menu.surf_.h + (margin + growth_pad) * 2 + 2;
    }
    static ID2D1Bitmap1* DesktopBitmap(const FluentMenu& menu) { return menu.backdrop_source_.Bitmap(); }
    static bool SampleShowsThrough(const FluentMenu& menu) {
        const auto& s = menu.surf_;
        const auto* pixels = static_cast<const DWORD*>(s.bits);
        const int y = s.h - FluentMenu::kShadowMargin - 4;
        return pixels && pixels[y * s.w + FluentMenu::kShadowMargin + 15] !=
            pixels[y * s.w + s.w - FluentMenu::kShadowMargin - 15];
    }
    static int Offset(const FluentMenu& menu) { return menu.present_offset_; }
    static int SubY(const FluentMenu& menu) { return menu.sub_y_; }
    static HWND Window(const FluentMenu& menu) { return menu.hwnd_; }
    static HWND SubWindow(const FluentMenu& menu) { return menu.sub_hwnd_; }
    static bool SubOpen(const FluentMenu& menu) { return menu.sub_parent_row_ == 0 && IsWindowVisible(menu.sub_hwnd_); }
    static bool Hidden(const FluentMenu& menu) { return !menu.open_ && !menu.animating_out_ &&
        !IsWindowVisible(menu.hwnd_) && !IsWindowVisible(menu.sub_hwnd_); }
    static bool HasChildren(const FluentMenu& menu, int count) { return menu.sub_model_.Count() == count; }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool ForegroundBelongsToAnotherProcess() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    // The user can switch external windows while this test is running.
    return process != GetCurrentProcessId();
}
pulse::ui::FluentMenuItem Item(int command, const wchar_t* text) {
    pulse::ui::FluentMenuItem item;
    item.command = command;
    item.text = text;
    return item;
}
bool SaveScreen(const std::filesystem::path& path, RECT rect) {
    using namespace pulse::ui;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT) != FALSE;
    SelectObject(memory, old);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> image;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    bool saved = copied && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image)) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr));
    if (saved) saved = SUCCEEDED(frame->WriteSource(image.get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
    return saved;
}
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDPIAware();
    using namespace pulse::ui;
    if (argc == 2 && wcscmp(argv[1], L"--popup-placement") == 0) {
        const RECT work{-1920, -100, 0, 940};
        const auto inside = ConstrainPopupOrigin({-1000, 200}, {300, 600}, work);
        Check(inside.x == -1000 && inside.y == 200, "popup preserves an interior anchor");
        const auto trailing = ConstrainPopupOrigin({-50, 900}, {300, 600}, work);
        Check(trailing.x == -300 && trailing.y == 340, "popup clamps trailing work-area edges");
        const auto oversized = ConstrainPopupOrigin({-50, 900}, {2200, 1250}, work);
        Check(oversized.x == work.left && oversized.y == work.top,
            "oversized popup uses ordered bounds at high DPI on a small work area");
        const auto leading = ConstrainPopupOrigin({-3000, -500}, {300, 600}, work);
        Check(leading.x == work.left && leading.y == work.top, "popup clamps leading edges on a negative monitor");
        for (const RECT area : {RECT{0, 0, 1920, 1040}, RECT{0, 0, 320, 240},
                                RECT{-300, -100, 0, 940}}) {
            const float scale = FitPopupScale(2.0f, 292.0f, 599.0f, 24, area);
            const SIZE size{static_cast<LONG>(std::lround(292.0f * scale)) + 48,
                            static_cast<LONG>(std::lround(599.0f * scale)) + 48};
            const auto origin = ConstrainPopupOrigin({area.right, area.bottom}, size, area);
            Check(scale >= 0.25f && scale <= 2.0f && size.cx <= area.right - area.left &&
                size.cy <= area.bottom - area.top, "fitted popup surface fits the small work area");
            Check(origin.y + 24 + 591.0f * scale <= area.bottom && origin.x >= area.left &&
                origin.x + size.cx <= area.right,
                "fitted popup accept row remains reachable at trailing work-area edge");
        }
        Check(FitPopupScale(1.0f, 292.0f, 599.0f, 24, RECT{0, 0, 1920, 1040}) == 1.0f,
            "fitting popup preserves requested DPI scale");
        return failures ? 1 : 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--command-state") == 0) {
        FluentMenuModel model;
        auto pending = Item(8001, L"Shell action");
        pending.enabled = false;
        auto parent = Item(0, L"Shell submenu");
        parent.enabled = false;
        parent.children.push_back(pending);
        model.SetItems({pending, parent});
        model.Layout(nullptr, 1.0f);
        const auto width = model.WidthPx(), height = model.HeightPx();
        auto live = pending;
        live.command = 8257; live.enabled = true;
        parent.enabled = true; parent.children = {live};
        Check(model.PatchCommands({live, parent}) && model.At(0)->command == live.command &&
            model.At(0)->enabled && model.At(1)->enabled && model.At(1)->children.front().enabled &&
            model.At(1)->children.front().command == live.command,
            "pending Shell rows and submenu commands enable when the current session arrives");
        Check(model.WidthPx() == width && model.HeightPx() == height,
            "enabling current commands preserves popup geometry");
        live.text = L"Different action";
        Check(!model.PatchCommands({live, parent}) && model.At(0)->text == pending.text,
            "command patch still refuses changed captions and preserves the displayed identity");
        return failures ? 1 : 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--live-refresh") == 0) {
        FluentMenuModel model;
        auto create = Item(186, L"New"); create.live_update = true;
        create.children = {Item(30, L"Folder"), Item(31, L"File")};
        auto stable = Item(1, L"Open");
        auto loading = Item(0, L"Loading"); loading.live_update = true; loading.enabled = false;
        model.SetItems({create, stable, loading}); model.Layout(nullptr, 1);
        const float stable_y = model.RowTopPx(1);
        create.children.push_back(Item(8012, L"Office document"));
        auto shell = Item(8013, L"Shell action"); shell.live_update = true;
        Check(model.ReplaceLiveItems({create, stable, shell}), "async template children and trailing extension section can refresh");
        model.Layout(nullptr, 1);
        Check(model.RowTopPx(1) == stable_y && model.At(0)->children.size() == 3,
            "refresh leaves the existing built-in row at its original position");
        stable.text = L"Changed command";
        Check(!model.ReplaceLiveItems({create, stable, shell}) && model.At(1)->text == L"Open",
            "live refresh still rejects changes to stable command identity");
        return failures ? 1 : 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW fixture_class{};
    fixture_class.hInstance = GetModuleHandleW(nullptr);
    fixture_class.lpszClassName = L"PulseMenuMaterialFixture";
    fixture_class.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            if (GetWindowLongPtrW(window, GWLP_USERDATA)) {
                for (int x = 0; x < 1000; x += 100) {
                    HBRUSH brush = CreateSolidBrush(x % 200 ? RGB(60, 170, 225) : RGB(220, 90, 160));
                    RECT stripe{x, 0, x + 100, 520};
                    FillRect(dc, &stripe, brush);
                    DeleteObject(brush);
                }
            }
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    };
    RegisterClassW(&fixture_class);
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        fixture_class.lpszClassName, L"", WS_POPUP,
        -30000, -30000, 700, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        Check(owner && compositor.Init(owner), "create menu rendering host");
        if (argc == 3 && (wcscmp(argv[1], L"--acrylic") == 0 ||
                         wcscmp(argv[1], L"--acrylic-transparent") == 0)) {
            const bool transparent = wcscmp(argv[1], L"--acrylic-transparent") == 0;
            HWND underlay = nullptr;
            if (transparent) {
                underlay = CreateWindowExW(WS_EX_NOACTIVATE, fixture_class.lpszClassName,
                    L"", WS_POPUP, 270, 170, 1000, 520, nullptr, nullptr,
                    GetModuleHandleW(nullptr), nullptr);
                SetWindowLongPtrW(underlay, GWLP_USERDATA, 1);
                SetWindowPos(underlay, HWND_TOPMOST, 270, 170, 1000, 520, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                UpdateWindow(underlay);
            }
            std::filesystem::create_directories(argv[2]);
            Check(SetWindowPos(owner, HWND_TOPMOST, 270, 170, 1000, 520,
                SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "position the material fixture");
            UpdateWindow(owner);
            DwmFlush();
            RECT owner_rect{};
            GetWindowRect(owner, &owner_rect);
            compositor.Resize(1000, 520);
            auto* background = compositor.Dc();
            ComPtr<ID2D1SolidColorBrush> stripe_brush;
            background->CreateSolidColorBrush(D2D1::ColorF(0), &stripe_brush);
            background->BeginDraw();
            background->Clear(D2D1::ColorF(0, transparent ? 0.0f : 1.0f));
            for (int x = 0; !transparent && x < 1000; x += 100) {
                stripe_brush->SetColor(D2D1::ColorF(x % 200 ? 0x3CAAE1 : 0xDC5AA0));
                background->FillRectangle(D2D1::RectF(static_cast<float>(x), 0,
                    static_cast<float>(x + 100), 520), stripe_brush.get());
            }
            Check(SUCCEEDED(background->EndDraw()), "draw the composition-backed background fixture");
            compositor.Present();
            Check(compositor.CaptureFrame(), "capture the composed frame for the menu fallback");
            DwmFlush();
            if (transparent) {
                ComPtr<ID2D1Bitmap1> readback;
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    compositor.FrameCopy()->GetPixelFormat(), 96, 96);
                D2D1_MAPPED_RECT mapped{};
                const bool readable = SUCCEEDED(background->CreateBitmap(D2D1::SizeU(1, 1),
                    nullptr, 0, props, &readback));
                const D2D1_RECT_U pixel{500, 250, 501, 251};
                const bool copied = readable && SUCCEEDED(readback->CopyFromBitmap(nullptr,
                    compositor.FrameCopy(), &pixel)) && SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
                Check(copied && mapped.bits[3] == 0, "GPU frame omits the separately composed desktop background");
                if (copied) readback->Unmap();
            }
            HDC screen = GetDC(nullptr);
            COLORREF exposed = CLR_INVALID;
            for (int attempt = 0; attempt < 50; ++attempt) {
                DwmFlush();
                exposed = GetPixel(screen, owner_rect.right - 10, owner_rect.bottom - 10);
                if (exposed == RGB(60, 170, 225)) break;
                Sleep(10);
            }
            if (exposed != RGB(60, 170, 225)) std::printf("fixture pixel: 0x%08lX\n", exposed);
            Check(exposed == RGB(60, 170, 225),
                "material fixture is exposed for screen capture");
            ReleaseDC(nullptr, screen);
            ValidateRect(owner, nullptr);
            for (const float scale : {1.0f, 1.5f}) {
                for (const bool dark : {false, true}) {
                    FluentMenu menu;
                    Check(menu.Create(owner, &compositor, scale), "create acrylic popup");
                    menu.SetTopmost(true);
                    menu.SetTheme(dark, D2D1::ColorF(0.0f, 0.47f, 0.83f));
                    auto create = Item(186, L"新建"); create.live_update = true;
                    create.children = {Item(30, L"文件夹"), Item(31, L"文本文档"),
                        Item(8013, L"Microsoft Word 文档")};
                    Check(FluentMenuTestPeer::Begin(menu, {create, Item(1, L"刷新"), Item(2, L"属性")}),
                        "render acrylic foreground");
                    FluentMenuTestPeer::FullyVisible(menu);
                    FluentMenuTestPeer::Open(menu);
                    const bool fallback = !MenuBackdropSource::Enabled();
                    Check(FluentMenuTestPeer::Acrylic(menu) == !fallback &&
                        FluentMenuTestPeer::Acrylic(menu, true) == !fallback,
                        "parent and submenu use sampled acrylic or the requested opaque fallback");
                    Check(FluentMenuTestPeer::DesktopSample(menu) == !fallback,
                        "capture the composited background before showing either popup");
                    Check(FluentMenuTestPeer::CroppedSample(menu, scale) == !fallback,
                        "desktop sample is cropped to the popup and blur margin");
                    Check(FluentMenuTestPeer::SampleShowsThrough(menu) == !fallback,
                        "menu pixels retain background color variation only with acrylic");
                    DwmFlush(); Sleep(200);
                    HDC popup_screen = GetDC(nullptr);
                    Check(GetPixel(popup_screen, 350, 250) != RGB(220, 90, 160),
                        "the composited popup is visible above the patterned underlay");
                    ReleaseDC(nullptr, popup_screen);
                    const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                        std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                    Check(SaveScreen(std::filesystem::path(argv[2]) / name, owner_rect),
                        "capture composited menu and submenu over a patterned desktop fixture");
                    auto* captured = FluentMenuTestPeer::DesktopBitmap(menu);
                    create.children.push_back(Item(8014, L"Microsoft Excel 工作表"));
                    Check(menu.ReplaceItems({create, Item(1, L"刷新"), Item(2, L"属性")}) &&
                        FluentMenuTestPeer::DesktopBitmap(menu) == captured &&
                        FluentMenuTestPeer::SampleShowsThrough(menu) == !fallback,
                        "live submenu growth reuses the clean background instead of capturing its own pixels");
                    menu.Dismiss();
                    Check(!FluentMenuTestPeer::DesktopSample(menu) && !FluentMenuTestPeer::Acrylic(menu),
                        "dismissal releases desktop pixels and the cached plate");
                }
            }
            ShowWindow(owner, SW_HIDE);
            DestroyWindow(owner);
            if (underlay) DestroyWindow(underlay);
            Check(ForegroundBelongsToAnotherProcess(), "acrylic menus do not take foreground activation");
            CoUninitialize();
            return failures ? 1 : 0;
        } else if (argc == 2 && wcscmp(argv[1], L"--lifecycle") == 0) {
            FluentMenu menu;
            Check(menu.Create(owner, &compositor, 1), "create animation and dismissal host");
            auto create = Item(186, L"New"); create.live_update = true;
            create.children = {Item(30, L"Folder"), Item(31, L"File")};
            auto stable = Item(1, L"Open");
            Check(FluentMenuTestPeer::Begin(menu, {create, stable}), "start partial-alpha menu frame");
            FluentMenuTestPeer::Hover(menu);
            Check(FluentMenuTestPeer::Alpha(menu) == 64 && FluentMenuTestPeer::Offset(menu) == -8,
                "hover redraw preserves the fade and slide state");
            FluentMenuTestPeer::Open(menu);
            const int sub_y = FluentMenuTestPeer::SubY(menu);
            Check(FluentMenuTestPeer::SubOpen(menu) && FluentMenuTestPeer::Alpha(menu) == 64,
                "opening a flyout during fade-in does not force an opaque parent");
            FluentMenuTestPeer::Progress(menu);
            Check(FluentMenuTestPeer::SubY(menu) == sub_y + 5,
                "flyout follows the parent slide without losing its anchor");
            FluentMenuTestPeer::Scroll(menu, 24.0f);
            Check(FluentMenuTestPeer::SubY(menu) == sub_y + 5 - 24,
                "flyout follows its visible parent row after menu scrolling");
            FluentMenuTestPeer::Scroll(menu, 0.0f);
            create.children.push_back(Item(8012, L"Office document"));
            Check(menu.ReplaceItems({create, stable}) && FluentMenuTestPeer::SubOpen(menu) &&
                FluentMenuTestPeer::HasChildren(menu, 3) && FluentMenuTestPeer::Alpha(menu) == 160,
                "late templates refresh an open flyout without closing it or restarting opacity");
            FluentMenuTestPeer::PendingResult(menu, 31);
            menu.DismissForDeactivation(owner);
            menu.DismissForDeactivation(FluentMenuTestPeer::SubWindow(menu));
            Check(menu.IsOpen() && FluentMenuTestPeer::Result(menu) == 31,
                "owner and menu-internal focus transfers preserve popup and pending selection");
            menu.DismissForDeactivation(reinterpret_cast<HWND>(1));
            Check(FluentMenuTestPeer::Hidden(menu) && FluentMenuTestPeer::Result(menu) == 0,
                "external deactivation immediately hides both windows and discards pending selection");
            Check(FluentMenuTestPeer::Begin(menu, {create, stable}), "reopen menu for cancellation message");
            FluentMenuTestPeer::Open(menu);
            FluentMenuTestPeer::PendingResult(menu, 31);
            SendMessageW(FluentMenuTestPeer::Window(menu), WM_CANCELMODE, 0, 0);
            Check(FluentMenuTestPeer::Hidden(menu) && FluentMenuTestPeer::Result(menu) == 0,
                "sent WM_CANCELMODE immediately hides both windows and rejects stale selection");
            Check(FluentMenuTestPeer::Begin(menu, {create, stable}), "reopen menu for application deactivation");
            FluentMenuTestPeer::Open(menu);
            FluentMenuTestPeer::PendingResult(menu, 31);
            SendMessageW(FluentMenuTestPeer::Window(menu), WM_ACTIVATEAPP, FALSE, 0);
            Check(FluentMenuTestPeer::Hidden(menu) && FluentMenuTestPeer::Result(menu) == 0,
                "sent application deactivation immediately discards pending selection");
            Check(FluentMenuTestPeer::Begin(menu, {create, stable}), "reopen menu for successful command selection");
            FluentMenuTestPeer::ChooseRow(menu, 1);
            Check(FluentMenuTestPeer::Hidden(menu) && FluentMenuTestPeer::Result(menu) == 1,
                "internal command selection dismisses immediately while preserving its returned command");
            SendMessageW(FluentMenuTestPeer::Window(menu), WM_CANCELMODE, 0, 0);
            Check(FluentMenuTestPeer::Result(menu) == 0,
                "external cancellation before invocation also clears a selection from an already hidden menu");
            Check(FluentMenuTestPeer::Begin(menu, {create, stable}), "reopen menu for submenu command selection");
            FluentMenuTestPeer::Open(menu);
            FluentMenuTestPeer::ChooseSubRow(menu, 1);
            Check(FluentMenuTestPeer::Hidden(menu) && FluentMenuTestPeer::Result(menu) == 31,
                "submenu window mouse coordinates select the expected command");
            DestroyWindow(owner);
            Check(ForegroundBelongsToAnotherProcess(), "menu lifecycle does not take foreground activation");
            CoUninitialize();
            return failures ? 1 : 0;
        }
        const auto long_item = Item(2,
            L"使用 Microsoft 画图进行编辑，带有很长文件名称的 Explorer 操作测试.png");
        for (const float scale : {1.0f, 1.25f, 1.5f}) {
            compositor.RecreateTextFormats(scale);
            FluentMenuModel model;
            auto inline_item = Item(3, L"Long filename in compact search column.txt");
            inline_item.shortcut = L"C:\\Search";
            inline_item.shortcut_inline = true;
            auto explicit_tip = long_item;
            explicit_tip.tooltip = L"Explicit command explanation";
            auto shortcut_item = long_item;
            shortcut_item.shortcut = L"Ctrl+Shift+E";
            auto swatches = long_item;
            swatches.quick_swatches.push_back({5, D2D1::ColorF(1, 0, 0)});
            model.SetItems({Item(1, L"打开"), long_item, inline_item, explicit_tip, shortcut_item, swatches});
            model.Layout(compositor.DwriteFactory(), scale);
            Check(model.WidthPx() <= static_cast<int>(std::ceil(320 * scale)),
                "long menu verbs retain the compact width cap");
            Check(!model.LabelTruncated(0) && model.TooltipText(0).empty(),
                "short labels do not gain a redundant tooltip");
            Check(model.LabelTruncated(1) && model.TooltipText(1) == long_item.text,
                "clamped label exposes its full Unicode text");
            Check(model.LabelTruncated(2) && model.TooltipText(2) == inline_item.text,
                "inline search column exposes its truncated filename");
            Check(model.TooltipText(3) == explicit_tip.tooltip,
                "explicit command tooltip takes precedence");
            Check(model.LabelTruncated(4), "shortcut chrome is reserved before checking labels");
            Check(!model.LabelTruncated(5), "swatch strip is not treated as a label");
            Check(!model.LabelTruncated(-1) && model.TooltipText(999).empty(),
                "invalid rows have no tooltip");
            const int fixed_width = model.WidthPx(), fixed_count = model.Count();
            Check(model.UpdateCommandState(2, L"Undo", false) && model.At(1)->text == L"Undo" &&
                !model.At(1)->enabled && model.Count() == fixed_count,
                "invalidated undo command changes its label and enabled state without leaving stale delete text");
            model.Layout(compositor.DwriteFactory(), scale, static_cast<float>(fixed_width));
            Check(model.WidthPx() == fixed_width && model.Count() == fixed_count && model.TooltipText(1).empty(),
                "live undo refresh preserves menu geometry and clears its obsolete tooltip");
            Check(!model.UpdateCommandState(999, L"Absent", false), "updating absent commands cannot add popup rows");
            model.SetItems({Item(1, L"Open")});
            Check(!model.LabelTruncated(0), "replacing rows clears previous truncation state");
            model.Layout(compositor.DwriteFactory(), scale);
            Check(!model.LabelTruncated(0), "relayout clears old long-label results");

            if (argc == 2) {
                std::filesystem::create_directories(argv[1]);
                FluentMenu menu;
                Check(menu.Create(owner, &compositor, scale), "create snapshot menu");
                auto submenu = long_item;
                submenu.children.push_back(Item(6, L"Open"));
                auto badge = long_item;
                badge.badge_text = L"提示";
                auto rename = Item(4, L"重命名"); rename.glyph = L"\xE8AC";
                auto actions = Item(0, L"");
                actions.quick_swatches.push_back({5, {}, false, false, L"\xE8AC"});
                for (const bool dark : {false, true}) {
                    menu.SetTheme(dark, D2D1::ColorF(0.0f, 0.47f, 0.83f));
                    const auto filename = std::wstring(L"menu_text_") +
                        std::to_wstring(static_cast<int>(scale * 100)) +
                        (dark ? L"_dark.png" : L"_light.png");
                    const auto output = std::filesystem::path(argv[1]) / filename;
                    Check(menu.SaveDebugSnapshot(output.c_str(),
                        {Item(1, L"使用 Microsoft 画图进行编辑"), long_item, shortcut_item,
                         inline_item, submenu, badge, rename, actions}, 1), "save menu text visual evidence");
                }
            }
        }
    }
    DestroyWindow(owner);
    Check(ForegroundBelongsToAnotherProcess(), "menu tests do not take foreground activation");
    CoUninitialize();
    return failures ? 1 : 0;
}
