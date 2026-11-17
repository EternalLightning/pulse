// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <uxtheme.h>

namespace {
LRESULT CALLBACK SyncEditProc(HWND edit, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& compositor = *reinterpret_cast<pulse::ui::Compositor*>(data);
    LRESULT result = 0;
    if (pulse::ui::HandleChildEditMessage(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
        D2D1::ColorF(0, 0, 0), static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), edit, message, wp, lp, result))
        return result;
    return pulse::ui::DefPresentedChildEditProc(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1),
        D2D1::ColorF(0, 0, 0), edit, message, wp, lp);
}

bool CaptureSyncSurface(HWND window, const std::filesystem::path& path) {
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool ok = false;
    if (bitmap && memory) {
        const auto old = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT)) {
            GdiFlush();
            const DWORD bytes = width * height * 4;
            BITMAPFILEHEADER file{};
            file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
            file.bfSize = file.bfOffBits + bytes;
            HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD written = 0;
            if (output != INVALID_HANDLE_VALUE) {
                ok = WriteFile(output, &file, sizeof(file), &written, nullptr) &&
                    WriteFile(output, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
                    WriteFile(output, pixels, bytes, &written, nullptr) && written == bytes;
                CloseHandle(output);
            }
        }
        SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    return ok;
}

void RunSurfaceSync(bool animations) {
    using namespace pulse::ui;
    if (!animations) { std::puts("[SKIP] surface animation sampling requires enabled system animations"); return; }
    const auto old_mode = typography::CurrentTextRenderMode();
    const auto folder = std::filesystem::absolute(L"bench_data/dialog_fade_sync");
    std::filesystem::create_directories(folder);
    for (int mode = 0; mode < 3; ++mode) {
        typography::SetTextRenderMode(mode == 1 ? typography::TextRenderMode::Sharp : typography::TextRenderMode::Auto);
        HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kFixtureClass, L"Pulse fade sync owner",
            WS_POPUP, 100, 100, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetPropW(owner, L"Pulse.Test.FadeSync", reinterpret_cast<HANDLE>(1));
        ShowWindow(owner, SW_SHOWNOACTIVATE); UpdateWindow(owner);
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kFixtureClass,
            L"Pulse input surface sync fixture", WS_POPUP, 160, 160, 320, 180,
            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetPropW(window, L"Pulse.Test.FadeSync", reinterpret_cast<HANDLE>(1));
        Compositor compositor;
        Check(compositor.Init(window), "initialize shared dialog and editor composition fixture");
        compositor.RecreateTextFormats(1);
        auto* dc = compositor.Dc();
        if (!dc) { DestroyWindow(window); DestroyWindow(owner); continue; }
        HWND edit = CreateChildEdit(window, L"\u2588\u2588\u2588\u2588");
        SetWindowTheme(edit, L"", L"");
        HFONT font = CreateFontW(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            0, 0, 0, 0, L"Segoe UI");
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        if (mode == 2) SetPropW(edit, L"Pulse.NativeEditFallback", reinterpret_cast<HANDLE>(1));
        SetWindowSubclass(edit, SyncEditProc, 91, reinterpret_cast<DWORD_PTR>(&compositor));
        SetWindowPos(edit, nullptr, 60, 60, 220, 40, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0, 1));
        pulse::ui::ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush);
        dc->FillRectangle(D2D1::RectF(16, 60, 36, 100), brush.get());
        Check(SUCCEEDED(dc->EndDraw()), "render dialog reference pixels before showing");
        compositor.Present();
        std::atomic_bool stop = false, closing = false, ready = false;
        unsigned entry_samples = 0, exit_samples = 0;
        int entry_difference = 0, exit_difference = 0;
        bool entry_capture = false, exit_capture = false;
        const auto prefix = std::to_wstring(mode);
        std::thread observer([&] {
            HDC screen = GetDC(nullptr);
            HDC line = CreateCompatibleDC(screen);
            HBITMAP pixels = CreateCompatibleBitmap(screen, 45, 1);
            const auto old = SelectObject(line, pixels);
            ready = true;
            while (!stop.load()) {
                DwmFlush();
                BitBlt(line, 0, 0, 45, 1, screen, 184, 236, SRCCOPY | CAPTUREBLT);
                const int body = GetRValue(GetPixel(line, 0, 0));
                const int text = GetRValue(GetPixel(line, 44, 0));
                if (body > 24 && body < 232) {
                    if (closing.load()) {
                        ++exit_samples; exit_difference = (std::max)(exit_difference, std::abs(body - text));
                        if (!exit_capture) exit_capture = CaptureSyncSurface(window, folder / (prefix + L"-close.bmp"));
                    } else {
                        ++entry_samples; entry_difference = (std::max)(entry_difference, std::abs(body - text));
                        if (!entry_capture) entry_capture = CaptureSyncSurface(window, folder / (prefix + L"-open.bmp"));
                    }
                }
            }
            SelectObject(line, old); DeleteObject(pixels); DeleteDC(line);
            ReleaseDC(nullptr, screen);
        });
        while (!ready.load()) std::this_thread::yield();
        ShowDialogWithFade(window, SW_SHOWNOACTIVATE);
        PresentChildEdit(compositor, compositor.TextFormat(), D2D1::ColorF(1, 1, 1), D2D1::ColorF(0, 0, 0), edit);
        PumpFade(window);
        const bool expected_custom = mode != 2;
        Check((GetPropW(edit, L"Pulse.EditComposition") != nullptr) == expected_custom,
            "exercise Auto text, Sharp text and native presentation fallback");
        CaptureSyncSurface(window, folder / (prefix + L"-settled.bmp"));
        BYTE final_alpha = 0; DWORD final_flags = 0;
        GetLayeredWindowAttributes(edit, nullptr, &final_alpha, &final_flags);
        std::printf("[SETTLED] mode=%d alpha=%u flags=%lu native=%d policy=%d\n", mode, final_alpha,
            final_flags, GetPropW(edit, L"Pulse.NativeEditFallback") != nullptr,
            GetPropW(edit, L"Pulse.NativeEditPolicy") != nullptr);
        closing = true;
        HideComposedDialog(window);
        stop = true; observer.join();
        std::printf("[SYNC] mode=%d entry=%u delta=%d exit=%u delta=%d\n", mode,
            entry_samples, entry_difference, exit_samples, exit_difference);
        Check(entry_samples > 0 && entry_difference <= 24, "input text and dialog pixels enter on the same opacity clock");
        Check(exit_samples > 0 && exit_difference <= 24, "input text and dialog pixels leave on the same opacity clock");
        Check(entry_capture && exit_capture, "capture actual composed entry and exit frames of the isolated fixture");
        Check(!GetPropW(window, L"Pulse.DialogChildOpacity") &&
            !GetPropW(edit, L"Pulse.DialogNativeEditSnapshot") && !IsWindowVisible(edit),
            "closing removes child fade state and hides the editor with the dialog");
        ShowDialogWithFade(window, SW_SHOWNOACTIVATE);
        HideComposedDialog(window);
        Check(!IsWindowVisible(window) && !GetPropW(window, L"Pulse.DialogFadeState"),
            "rapid reopen and close leaves no partial dialog or editor animation");
        compositor.Shutdown(); DestroyWindow(window); DeleteObject(font); DestroyWindow(owner);
    }
    typography::SetTextRenderMode(old_mode);
}
}
