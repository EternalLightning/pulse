#include "../app/app_internal.h"
#include "../app/background_window_fit.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void Check(bool ok, const char* text) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
    if (!ok) ++failures;
}

LRESULT CALLBACK FixtureProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
        limits->ptMinTrackSize = {320, 420};
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool WriteImage(const std::filesystem::path& path, LONG width, LONG height) {
    const DWORD stride = (static_cast<DWORD>(width) * 3 + 3) & ~3u;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + stride * height;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    std::vector<BYTE> pixels(stride * height);
    for (LONG y = 0; y < height; ++y) for (LONG x = 0; x < width; ++x) {
        const size_t offset = static_cast<size_t>(y) * stride + x * 3;
        pixels[offset] = static_cast<BYTE>(90 + 90 * x / width);
        pixels[offset + 1] = static_cast<BYTE>(65 + 70 * y / height);
        pixels[offset + 2] = static_cast<BYTE>(40 + 90 * x / width);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    return out.good();
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    using H = ui::HitTestResult;
    using I = l10n::StringId;
    const auto output = std::filesystem::absolute(argc > 1 ? argv[1] : "bench_data/background-window-fit");
    std::filesystem::create_directories(output);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    for (LONG scale : {1L, 2L}) {
        for (SIZE image : {SIZE{160, 90}, SIZE{90, 160}, SIZE{1, 1}, SIZE{100000, 1}, SIZE{1, 100000}}) {
            const SIZE frame{16 * scale, 8 * scale};
            const SIZE minimum{320 * scale, 420 * scale};
            const SIZE maximum{1920 * scale, 1040 * scale};
            const auto size = app::FitBackgroundWindowSize(image, {1100 * scale, 720 * scale}, frame, minimum, maximum);
            Check(size.cx >= minimum.cx && size.cy >= minimum.cy && size.cx <= maximum.cx && size.cy <= maximum.cy,
                "landscape/portrait/square/extreme ratios respect minimum and work-area limits at 100/200 percent");
            if (image.cx < 100000 && image.cy < 100000)
                Check(std::abs(static_cast<double>(size.cx - frame.cx) / (size.cy - frame.cy) -
                    static_cast<double>(image.cx) / image.cy) < 0.004,
                    "client-area aspect ratio matches image within pixel rounding");
        }
    }
    Check(app::FitBackgroundWindowSize({}, {100, 100}, {}, {320, 420}, {1920, 1040}).cx == 0,
        "invalid dimensions do not produce a resize");
    Check(app::FitBackgroundWindowSize({160, 90}, {100, 100}, {16, 8}, {320, 420}, {16, 8}).cx == 0,
        "invalid work area does not produce a resize");
    const auto landscape = output / L"landscape.bmp", portrait = output / L"portrait.bmp";
    Check(WriteImage(landscape, 160, 90) && WriteImage(portrait, 90, 160), "isolated background fixtures written");
    const auto broken = output / L"broken.bmp";
    std::ofstream(broken) << "not an image";

    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.ctxMenuPrefs.persist = false;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = FixtureProc;
    wc.lpszClassName = L"PulseBackgroundFitFixture";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Background fit fixture", WS_OVERLAPPEDWINDOW,
        200, 160, 1100, 800, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = app::MakeSettingsPath(app::SettingsController::PageName(0));
    s.settings.SelectPage(0);
    s.settingsExpanded |= 1u;
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    H action; action.region = H::SettingsWallpaper; action.index = 2;
    RECT before{}, after{};
    GetWindowRect(s.hwnd, &before);
    HandleSettingsControl(s, action);
    GetWindowRect(s.hwnd, &after);
    Check(EqualRect(&before, &after), "empty background leaves the window unchanged");
    auto wait_size = [&](const std::wstring& path, LONG width, LONG height) {
        const auto deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline) {
            const auto size = s.renderer.BackgroundImageSize(path);
            if (size.cx == width && size.cy == height) return true;
            Sleep(10);
        }
        return false;
    };
    Check(s.renderer.BackgroundImageSize(landscape.wstring()).cx == 0,
        "first background query is asynchronous and has no stale dimensions");
    Check(wait_size(landscape.wstring(), 160, 90), "asynchronous landscape dimensions become available");
    Check(s.renderer.BackgroundImageSize(portrait.wstring()).cx == 0,
        "switching image does not reuse the previous image dimensions");
    Check(wait_size(portrait.wstring(), 90, 160), "asynchronous portrait dimensions become available");
    s.appPrefs.background_image = portrait.wstring();
    HandleSettingsControl(s, action);
    RECT client{}; GetClientRect(s.hwnd, &client);
    Check(std::abs(static_cast<double>(client.right) / client.bottom - 90.0 / 160) < 0.004,
        "actual settings action resizes a native window to portrait client ratio");
    ShowWindow(s.hwnd, SW_MAXIMIZE);
    s.appPrefs.background_image = landscape.wstring();
    Check(wait_size(landscape.wstring(), 160, 90), "landscape reload completes after maximization");
    HandleSettingsControl(s, action);
    GetClientRect(s.hwnd, &client); GetWindowRect(s.hwnd, &after);
    Check(!IsZoomed(s.hwnd) && std::abs(static_cast<double>(client.right) / client.bottom - 160.0 / 90) < 0.004,
        "matching a maximized window restores it and applies landscape ratio");
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    Check(after.left >= monitor.rcWork.left && after.top >= monitor.rcWork.top &&
        after.right <= monitor.rcWork.right && after.bottom <= monitor.rcWork.bottom,
        "actual resized window stays in the monitor work area");
    SetWindowPos(s.hwnd, nullptr, after.left, after.top, 800, 650, SWP_NOZORDER | SWP_NOACTIVATE);
    GetWindowRect(s.hwnd, &after);
    Check(after.right - after.left == 800 && after.bottom - after.top == 650,
        "later user resizing remains free of any aspect-ratio lock");
    ShowWindow(s.hwnd, SW_HIDE);

    struct View { float width, scale; bool dark; };
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) for (const View view : {
            View{760, 1, false}, View{760, 1.5f, true}, View{1280, 1, true}, View{1280, 2, false}}) {
        l10n::SetLanguage(language);
        s.scale = view.scale; s.darkMode = view.dark;
        s.renderer.SetScale(view.scale); s.compositor.RecreateTextFormats(view.scale); painter.SetScale(view.scale);
        const auto rect = D2D1::RectF(0, 0, view.width * view.scale, 700 * view.scale);
        s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
        auto vm = BuildVm(s, false); vm.settings_scroll = 0;
        auto layout = ui::MakeSettingsLayout(vm, rect, view.scale, s.renderer.TitleBarHeight(), 28 * view.scale, &painter);
        vm.settings_scroll = layout.wallpaper_card.top - layout.content.top - 16 * view.scale;
        layout = ui::MakeSettingsLayout(vm, rect, view.scale, s.renderer.TitleBarHeight(), 28 * view.scale, &painter);
        const auto& row = layout.wallpaper_fit_card;
        const auto& button = layout.wallpaper_fit;
        Check(row.top >= layout.wallpaper_card.bottom && row.bottom <= layout.wallpaper_look_card.top &&
            button.left >= row.left + 16 * view.scale && button.right <= row.right - 16 * view.scale &&
            button.top >= row.top && button.bottom <= row.bottom,
            "background match row follows chooser and its control fits in narrow/wide layouts");
        const bool stacked = button.left < row.left + 100 * view.scale;
        const float text_right = stacked ? row.right - 16 * view.scale : button.left - 12 * view.scale;
        auto fits = [&](I id, IDWriteTextFormat* format, float width, float height) {
            const auto& value = l10n::Get(id);
            ui::ComPtr<IDWriteTextLayout> measured;
            if (value.empty() || FAILED(s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(),
                static_cast<UINT32>(value.size()), format, 4000 * view.scale, 100 * view.scale, &measured))) return false;
            DWRITE_TEXT_METRICS metrics{}; measured->GetMetrics(&metrics);
            const bool fit = metrics.widthIncludingTrailingWhitespace <= width + 1 && metrics.height <= height + 1;
            if (!fit) std::cout << "text id=" << static_cast<int>(id) << " measured=" << metrics.widthIncludingTrailingWhitespace
                << 'x' << metrics.height << " bounds=" << width << 'x' << height << std::endl;
            return fit;
        };
        Check(fits(I::SettingsMatchBackground, s.compositor.TextFormat(), text_right - row.left - 54 * view.scale, 24 * view.scale) &&
            fits(I::SettingsMatchBackgroundDesc, s.compositor.SmallFormat(), text_right - row.left - 54 * view.scale, 21 * view.scale) &&
            fits(I::MatchWindowSize, s.compositor.TextFormat(), button.right - button.left - 16 * view.scale, 24 * view.scale),
            "Chinese/English title, description and button fit at 100/150/200 percent DPI");
        const auto hit = s.renderer.HitTest(vm, rect, (button.left + button.right) / 2, (button.top + button.bottom) / 2);
        Check(hit.region == H::SettingsWallpaper && hit.index == 2, "enabled image match button exposes its own hit target");
        const auto theme = ui::MakeTheme(view.dark, s.accentColor);
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, rect, theme);
        Check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "background match settings render without Direct2D errors");
        const auto file = output / (std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(view.width)) + L"-" +
            std::to_wstring(static_cast<int>(view.scale * 100)) + (view.dark ? L"-dark.png" : L"-light.png"));
        Check(s.compositor.SaveSnapshot(file.c_str()), "background settings screenshot captured before Present");
        vm.background_image.clear();
        const auto disabled = s.renderer.HitTest(vm, rect, (button.left + button.right) / 2, (button.top + button.bottom) / 2);
        Check(disabled.region != H::SettingsWallpaper, "empty background disables the action hit target");
        vm.background_image = broken.wstring();
        const auto invalid = s.renderer.HitTest(vm, rect, (button.left + button.right) / 2, (button.top + button.bottom) / 2);
        Check(invalid.region != H::SettingsWallpaper, "invalid or loading image disables the action hit target");
        Check(wait_size(landscape.wstring(), 160, 90), "valid image can be selected after an invalid image request");
    }
    s.settings.ResetUi(); s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    state.reset(); CoUninitialize();
    std::cout << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
