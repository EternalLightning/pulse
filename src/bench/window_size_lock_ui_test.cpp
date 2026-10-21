#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

LRESULT CALLBACK WndProcImpl(HWND, UINT, WPARAM, LPARAM);

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
LRESULT CALLBACK FixtureProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (GetWindowLongPtrW(hwnd, GWLP_USERDATA)) {
        switch (message) {
        case WM_WINDOWPOSCHANGING: case WM_SYSCOMMAND: case WM_GETMINMAXINFO:
        case WM_NCHITTEST: case WM_NCCALCSIZE: case WM_DPICHANGED:
            return WndProcImpl(hwnd, message, wp, lp);
        }
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
RECT WindowRect(HWND hwnd) {
    RECT rect{}; GetWindowRect(hwnd, &rect); return rect;
}
bool SameSize(RECT a, RECT b) {
    return a.right - a.left == b.right - b.left && a.bottom - a.top == b.bottom - b.top;
}
}

int RunWindowSizeLockUiTest() {
    using namespace pulse;
    using I = l10n::StringId;
    using H = ui::HitTestResult;
    const auto output = std::filesystem::absolute(L"bench_data/window-size-lock-ui");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true; s.shot.active = true;
    s.accentColor = ui::HexColor(0x0078D4);
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    app::SettingsController::UiCallbacks callbacks;
    callbacks.apply_effects = [&s](app::SettingsEffect effect) { ApplySettingsEffects(s, effect); };
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = app::MakeSettingsPath(app::SettingsController::PageName(0));
    ActiveTab(s)->loading = false;
    s.settings.SelectPage(0); s.settingsExpanded |= 1u;
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = FixtureProc; wc.lpszClassName = L"PulseWindowSizeLockFixture";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Window size lock fixture",
        WS_OVERLAPPEDWINDOW, 200, 160, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) { Check(false, "create window and compositor"); return 1; }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.renderer.SetCompositor(&s.compositor);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    const auto original = WindowRect(s.hwnd);
    s.settings.ToggleUi(34);
    Check(s.appPrefs.lock_window_size, "settings controller enables size lock");
    app::AppPrefs parsed; parsed.persist = false;
    Check(parsed.FromJson(s.appPrefs.ToJson()) && parsed.lock_window_size,
        "size lock survives preferences JSON roundtrip");
    Check(parsed.FromJson(L"{}") && !parsed.lock_window_size, "older preferences default to unlocked");
    for (const POINT point : {POINT{original.left + 1, original.top + 1},
            POINT{original.right - 1, original.bottom - 1},
            POINT{original.left + 1, (original.top + original.bottom) / 2},
            POINT{(original.left + original.right) / 2, original.bottom - 1}}) {
        Check(SendMessageW(s.hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCLIENT,
            "locked border and corner cannot start a resize");
    }
    SetWindowPos(s.hwnd, nullptr, 240, 200, 1150, 810, SWP_NOZORDER | SWP_NOACTIVATE);
    auto after = WindowRect(s.hwnd);
    Check(SameSize(original, after) && after.left == 240 && after.top == 200,
        "position changes remain allowed while size changes are blocked");
    Check(SendMessageW(s.hwnd, WM_SYSCOMMAND, SC_SIZE | WMSZ_BOTTOMRIGHT, 0) == 0 &&
        SameSize(original, WindowRect(s.hwnd)), "system Size command is blocked without entering its modal loop");
    SendMessageW(s.hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    Check(IsZoomed(s.hwnd) && !SameSize(original, WindowRect(s.hwnd)), "system maximize is allowed");
    ShowWindow(s.hwnd, SW_RESTORE);
    Check(!IsZoomed(s.hwnd) && SameSize(original, WindowRect(s.hwnd)), "restore returns to the locked size");
    ShowWindow(s.hwnd, SW_MAXIMIZE);
    Check(IsZoomed(s.hwnd), "caption-button ShowWindow maximize is allowed");
    ShowWindow(s.hwnd, SW_MINIMIZE);
    Check(IsIconic(s.hwnd), "minimizing a maximized locked window is allowed");
    ShowWindow(s.hwnd, SW_RESTORE);
    Check(IsZoomed(s.hwnd), "restoring a minimized maximized window retains maximization");
    ShowWindow(s.hwnd, SW_RESTORE);
    Check(SameSize(original, WindowRect(s.hwnd)), "second restore retains the original normal size");
    ShowWindow(s.hwnd, SW_MINIMIZE); ShowWindow(s.hwnd, SW_RESTORE);
    Check(SameSize(original, WindowRect(s.hwnd)), "normal minimize and restore preserve the locked size");
    s.settings.ToggleUi(34);
    SetWindowPos(s.hwnd, nullptr, 0, 0, 900, 650, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const auto resized = WindowRect(s.hwnd);
    Check(!s.appPrefs.lock_window_size && resized.right - resized.left == 900 &&
        resized.bottom - resized.top == 650, "turning the lock off immediately restores resizing");
    ShowWindow(s.hwnd, SW_MAXIMIZE); s.settings.ToggleUi(34); ShowWindow(s.hwnd, SW_RESTORE);
    Check(SameSize(resized, WindowRect(s.hwnd)), "enabling while maximized locks the restored normal size");
    RECT dpi_rect{250, 250, 1600, 1200};
    SendMessageW(s.hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&dpi_rect));
    Check(SameSize(resized, WindowRect(s.hwnd)), "DPI transition preserves locked pixel dimensions");
    s.settings.ToggleUi(34);
    parsed.lock_window_size = true; parsed.ResetToDefaults();
    Check(!parsed.lock_window_size, "reset preferences removes the lock");
    ShowWindow(s.hwnd, SW_HIDE);

    // Use a real asynchronously decoded image so the disabled match control is meaningful.
    const auto image = output / L"background.bmp";
    BITMAPFILEHEADER file{}; file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + 16;
    BITMAPINFOHEADER bitmap{}; bitmap.biSize = sizeof(bitmap); bitmap.biWidth = bitmap.biHeight = 2;
    bitmap.biPlanes = 1; bitmap.biBitCount = 24;
    { std::ofstream stream(image, std::ios::binary); const char pixels[16]{};
      stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
      stream.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap)); stream.write(pixels, sizeof(pixels)); }
    s.appPrefs.background_image = image.wstring();
    const auto deadline = GetTickCount64() + 5000;
    while (s.renderer.BackgroundImageSize(image.wstring()).cx <= 0 && GetTickCount64() < deadline) Sleep(10);
    Check(s.renderer.BackgroundImageSize(image.wstring()).cx == 2, "load isolated background for match-control checks");
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"en-US"}) for (const float scale : {1.0f, 1.5f}) {
        l10n::SetLanguage(language);
        s.scale = scale; s.darkMode = scale > 1;
        s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale); painter.SetScale(scale);
        const auto bounds = D2D1::RectF(0, 0, (scale > 1 ? 760 : 1280) * scale, 800 * scale);
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(bounds.right), static_cast<int>(bounds.bottom),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(static_cast<UINT>(bounds.right), static_cast<UINT>(bounds.bottom));
        auto vm = BuildVm(s, false); vm.settings_scroll = 0;
        auto layout = ui::MakeSettingsLayout(vm, bounds, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
        s.settings.SetScroll(layout.wallpaper_fit_card.top - layout.content.top - 16 * scale, 100000);
        vm = BuildVm(s, false);
        layout = ui::MakeSettingsLayout(vm, bounds, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
        const auto row = layout.window_size_lock_row;
        const auto text = ui::SettingsToggleBounds(row, l10n::Get(I::SettingsLockWindowSizeDesc), scale, &painter);
        Check(row.top >= layout.wallpaper_fit_card.bottom && row.bottom <= layout.wallpaper_look_card.top &&
            text.description.bottom <= row.bottom && text.description.right <= text.control.left,
            "lock row follows match action and text fits beside the switch");
        const int x = static_cast<int>((text.control.left + text.control.right) / 2);
        const int y = static_cast<int>((text.control.top + text.control.bottom) / 2);
        const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
        Check(hit.region == H::SettingsToggle && hit.index == 34, "visible lock switch has its own production hit target");
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        vm = BuildVm(s, false);
        Check(s.appPrefs.lock_window_size && vm.settings_lock_window_size, "clicking the actual settings switch enables the lock");
        const auto match = layout.wallpaper_fit;
        const auto disabled = s.renderer.HitTest(vm, bounds, (match.left + match.right) / 2, (match.top + match.bottom) / 2);
        Check(disabled.region != H::SettingsWallpaper, "size match action is disabled while locked despite a valid image");
        H action; action.region = H::SettingsWallpaper; action.index = 2;
        const auto locked = WindowRect(s.hwnd); HandleSettingsControl(s, action);
        Check(SameSize(locked, WindowRect(s.hwnd)), "direct size-match dispatch also respects the lock");
        Render(s);
        const auto name = std::wstring(language) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
        Check(s.compositor.SaveSnapshot((output / name).c_str()), "capture lock setting with adjacent match action");
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        Check(!s.appPrefs.lock_window_size, "clicking the switch again unlocks the window");
        vm = BuildVm(s, false);
        const auto enabled = s.renderer.HitTest(vm, bounds, (match.left + match.right) / 2, (match.top + match.bottom) / 2);
        Check(enabled.region == H::SettingsWallpaper && enabled.index == 2,
            "unlocking restores the existing size-match action");
    }
    s.settings.ResetUi(); s.watches.Stop(); s.renderer.SetCompositor(nullptr);
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    if (s.editFont) { DeleteObject(s.editFont); s.editFont = nullptr; }
    std::printf("window size lock: %d failures\n", failures);
    return failures ? 1 : 0;
}
