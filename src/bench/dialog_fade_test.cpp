// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/new_item_dialog.h"
#include "../app/archive_dialog.h"
#include "../common/localization.h"
#include "../ui/FluentTokens.h"
#include "../common/windows_compat.h"
#include "../ui/dialog_lifecycle.h"
#include "../ui/ui_compositor.h"
#include "../ui/typography.h"
#include "../ui/input_dialog.h"
#include "../ui/edit_host.h"
#include "snapshot_output.h"
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>

namespace {
std::atomic_int failures = 0;
COLORREF prompt_accent = RGB(80, 180, 220);
std::atomic_int paints = 0;
int messages_during_fade = 0;
constexpr UINT kResponsivenessProbe = WM_APP + 8;
constexpr wchar_t kFixtureClass[] = L"PulseDialogFadeTestFixture";

void Check(bool condition, const char* description) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) failures.fetch_add(1);
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
LRESULT CALLBACK FixtureProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kResponsivenessProbe) {
        if (GetPropW(window, L"Pulse.DialogFadeState")) ++messages_during_fade;
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(48, 120, 184));
        FillRect(dc, &client, background);
        DeleteObject(background);
        RECT marker{40, 40, 180, 100};
        HBRUSH foreground = CreateSolidBrush(RGB(240, 184, 48));
        FillRect(dc, &marker, foreground);
        DeleteObject(foreground);
        EndPaint(window, &paint);
        paints.fetch_add(1);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

// Read only the fixture's client DC; never capture the desktop or another window.
bool CaptureFixture(HWND window, const wchar_t* filename) {
    pulse::ui::InputDialogInspection inspection;
    if (SendMessageW(window, pulse::ui::kInputDialogInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection))) {
        wchar_t fixture_root[32768]{};
        GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", fixture_root, ARRAYSIZE(fixture_root));
        const auto folder = fixture_root[0] ? std::filesystem::path(fixture_root) : std::filesystem::path(L"bench_data/dialog_fade");
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        auto output = std::filesystem::absolute(folder / filename);
        output.replace_extension(L".png");
        return !error && SendMessageW(window, pulse::ui::kInputDialogSnapshotMessage, 0,
            reinterpret_cast<LPARAM>(output.c_str())) != 0;
    }
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    const int width = client.right, height = client.bottom;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC source = GetDC(window);
    if (!source) return false;
    HDC memory = CreateCompatibleDC(source);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool good = false;
    if (memory && bitmap && pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY)) {
            const auto* bytes = static_cast<const BYTE*>(pixels);
            size_t colored = 0;
            const size_t count = static_cast<size_t>(width) * height;
            for (size_t index = 0; index < count; ++index) {
                if (bytes[index * 4] > 24 || bytes[index * 4 + 1] > 24 ||
                    bytes[index * 4 + 2] > 24) ++colored;
            }
            wchar_t fixture_root[32768]{};
            GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", fixture_root, ARRAYSIZE(fixture_root));
            const std::filesystem::path folder = fixture_root[0] ?
                std::filesystem::path(fixture_root) : std::filesystem::path(L"bench_data/dialog_fade");
            auto output = folder / filename;
            if (pulse::bench::StreamSnapshots()) {
                output.replace_extension(L".png");
                auto* opaque_pixels = static_cast<BYTE*>(pixels);
                for (size_t index = 0; index < count; ++index) opaque_pixels[index * 4 + 3] = 255;
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                good = colored > count / 2 && pulse::bench::EmitSnapshot(output,
                    static_cast<UINT>(width), static_cast<UINT>(height),
                    static_cast<UINT>(width) * 4, opaque_pixels);
                if (SUCCEEDED(initialized)) CoUninitialize();
            } else {
                std::error_code error;
                std::filesystem::create_directories(folder, error);
                HANDLE file = error ? INVALID_HANDLE_VALUE : CreateFileW(output.c_str(),
                    GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    const DWORD byte_count = static_cast<DWORD>(count * 4);
                    BITMAPFILEHEADER header{};
                    header.bfType = 0x4d42;
                    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
                    header.bfSize = header.bfOffBits + byte_count;
                    DWORD written = 0;
                    good = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
                        written == sizeof(header) &&
                        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
                        written == sizeof(info.bmiHeader) &&
                        WriteFile(file, pixels, byte_count, &written, nullptr) && written == byte_count &&
                        colored > count / 2;
                    CloseHandle(file);
                }
            }
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(window, source);
    return good;
}

struct Samples {
    std::vector<BYTE> alpha;
    std::vector<BYTE> dim_alpha;
    bool closing_seen = false;
    bool composed_close_seen = false;
    bool capture_attempted = false;
    bool capture_ok = false;
};

std::thread Sample(HWND window, std::atomic_bool& stop, std::atomic_bool& ready,
    Samples& samples, const wchar_t* screenshot, HWND dim = nullptr) {
    return std::thread([window, &stop, &ready, &samples, screenshot, dim] {
        ready.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            if (GetPropW(window, L"Pulse.DialogClosing")) {
                samples.closing_seen = true;
                samples.composed_close_seen |= pulse::ui::IsDialogSurfaceFading(window) &&
                    !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED);
                BYTE dim_alpha = 0;
                DWORD dim_flags = 0;
                if (dim && GetLayeredWindowAttributes(dim, nullptr, &dim_alpha, &dim_flags) &&
                    dim_alpha > 0 && dim_alpha < 64) samples.dim_alpha.push_back(dim_alpha);
            }
            BYTE alpha = 0;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA) && alpha > 0 && alpha < 255) {
                samples.alpha.push_back(alpha);
                if (screenshot && !samples.capture_attempted && alpha >= 64 && alpha <= 224 &&
                    paints.load() > 0) {
                    samples.capture_attempted = true;
                    samples.capture_ok = CaptureFixture(window, screenshot);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

bool AnimationsEnabled() {
    BOOL enabled = TRUE;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    Check(SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != FALSE,
        "read client-area animation setting without modifying it");
    Check(SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) != FALSE,
        "read high-contrast setting without modifying it");
    return enabled && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
}

void PumpFade(HWND window) {
    const auto deadline = GetTickCount64() + 1500;
    while (IsWindow(window) && GetPropW(window, L"Pulse.DialogFadeState") &&
        GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    Check(!GetPropW(window, L"Pulse.DialogFadeState"), "fade completes through ordinary message dispatch");
}

void RunNative(HWND owner, bool animations) {
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kFixtureClass, L"Pulse fade fixture",
        WS_POPUP, 160, 160, 320, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create isolated native GDI dialog");
    if (!window) return;
    const LONG_PTR original_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    Check(paints.load() > 0, "native fixture handles real WM_PAINT");
    Check(CaptureFixture(window, L"native-before.bmp"), "before: save nonblack native client screenshot");
    ShowWindow(window, SW_HIDE);
    std::atomic_bool stop = false, ready = false;
    Samples showing;
    auto helper = Sample(window, stop, ready, showing, L"native-midpoint.bmp");
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    pulse::ui::ShowDialogWithFade(window);
    Check(!animations || GetPropW(window, L"Pulse.DialogFadeState"),
        "fade-in returns while animation remains pending");
    PostMessageW(window, kResponsivenessProbe, 0, 0);
    PumpFade(window);
    stop.store(true, std::memory_order_release);
    helper.join();
    Check(IsWindowVisible(window) != FALSE, "fade-in leaves native dialog visible");
    Check(GetWindowLongPtrW(window, GWL_EXSTYLE) == original_style,
        "fade-in restores original extended style");
    Check(CaptureFixture(window, L"native-after.bmp"), "after: save nonblack native client screenshot");
    if (animations) {
        Check(messages_during_fade > 0, "UI dispatches posted messages during fade-in");
        Check(!showing.alpha.empty(), "fade-in samples alpha strictly between 0 and 255");
        Check(showing.capture_attempted && showing.capture_ok,
            "midpoint: save nonblack native client screenshot during fade-in");
        if (showing.alpha.size() > 1)
            Check(showing.alpha.front() < showing.alpha.back(), "fade-in opacity increases");
    } else {
        Check(showing.alpha.empty(), "disabled animations show immediately without layered transition");
        std::printf("[SKIP] Intermediate alpha/screenshot sampling: system animations disabled or high contrast enabled.\n");
    }
    stop.store(false, std::memory_order_release);
    ready.store(false, std::memory_order_release);
    Samples closing;
    helper = Sample(window, stop, ready, closing, L"native-close-midpoint.bmp");
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    pulse::ui::DestroyDialogWithFade(window);
    stop.store(true, std::memory_order_release);
    helper.join();
    Check(!IsWindow(window), "closing destroys native dialog before returning");
    if (animations) {
        Check(closing.closing_seen && closing.alpha.size() > 1 &&
            closing.alpha.front() > closing.alpha.back(),
            "fade-out presents intermediate frames with decreasing native opacity");
        Check(closing.capture_attempted && closing.capture_ok,
            "close midpoint: save nonblack native client screenshot before destruction");
    } else Check(closing.alpha.empty(), "disabled animations close immediately without layered transition");

    window = CreateWindowExW(WS_EX_TOOLWINDOW, kFixtureClass, L"Pulse quick close fixture",
        WS_POPUP, 160, 160, 320, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    pulse::ui::ShowDialogWithFade(window);
    pulse::ui::DestroyDialogWithFade(window, owner);
    Check(!IsWindow(window) && IsWindowEnabled(owner),
        "rapid close cancels pending native fade and restores modal owner");
    window = CreateWindowExW(WS_EX_TOOLWINDOW, kFixtureClass, L"Pulse direct destruction fixture",
        WS_POPUP, 160, 160, 320, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    pulse::ui::ShowDialogWithFade(window);
    DestroyWindow(window);
    Check(!IsWindow(window), "direct HWND destruction cancels pending fade without an explicit hide");
    window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kFixtureClass,
        L"Pulse unbound no-redirection fixture", WS_POPUP, 160, 160, 320, 180,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    pulse::ui::ShowDialogWithFade(window, SW_SHOWNOACTIVATE);
    Check(IsWindowVisible(window) && !GetPropW(window, L"Pulse.DialogFadeState") &&
        !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED),
        "unbound no-redirection window shows immediately without incompatible layering");
    DestroyWindow(window);
}

void RunStyleChanges(HWND owner, bool animations) {
    if (!animations) {
        std::printf("[SKIP] Concurrent style changes: system animations disabled or high contrast enabled.\n");
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, kFixtureClass, L"Pulse style change fixture",
            WS_POPUP, 160, 160, 320, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create native style-change fixture");
        if (!window) continue;
        pulse::ui::ShowDialogWithFade(window, SW_SHOWNOACTIVATE);
        Check(GetPropW(window, L"Pulse.DialogFadeState") != nullptr,
            "change extended styles while native fade remains pending");
        const LONG_PTR updated_style = (GetWindowLongPtrW(window, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) |
            WS_EX_NOACTIVATE;
        SetWindowLongPtrW(window, GWL_EXSTYLE, updated_style);
        if (pass == 0) PumpFade(window);
        else pulse::ui::HideComposedDialog(window);
        Check(GetWindowLongPtrW(window, GWL_EXSTYLE) == (updated_style & ~WS_EX_LAYERED),
            pass == 0 ? "completed fade preserves concurrent extended-style additions and removals"
                : "cancelled fade preserves concurrent extended-style additions and removals");
        DestroyWindow(window);
    }
}

void RunLayered(HWND owner) {
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kFixtureClass,
        L"Pulse layered fade fixture", WS_POPUP, 160, 160, 320, 180,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create existing layered dialog");
    if (!window) return;
    constexpr COLORREF key = RGB(1, 2, 3);
    SetLayeredWindowAttributes(window, key, 192, LWA_ALPHA | LWA_COLORKEY);
    pulse::ui::ShowDialogWithFade(window);
    pulse::ui::HideComposedDialog(window);
    COLORREF restored_key = 0;
    BYTE restored_alpha = 0;
    DWORD restored_flags = 0;
    Check(!IsWindowVisible(window) && IsWindow(window), "fade-hide preserves native HWND");
    Check(GetLayeredWindowAttributes(window, &restored_key, &restored_alpha, &restored_flags) &&
        restored_key == key && restored_alpha == 192 && restored_flags == (LWA_ALPHA | LWA_COLORKEY),
        "fade restores existing layered alpha and color key");
    Check(!GetPropW(window, L"Pulse.DialogFadeState") && !GetPropW(window, L"Pulse.DialogClosing"),
        "completed close removes fade timer state and reentrancy guard");
    pulse::ui::ShowDialogWithFade(window);
    Check(IsWindowVisible(window) != FALSE, "hidden dialog can fade in again");
    pulse::ui::DestroyDialogWithFade(window);
}

void RunComposed(HWND owner, bool animations) {
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kFixtureClass, L"Pulse composition fade fixture", WS_POPUP, 160, 160, 320, 180,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create DirectComposition fixture");
    if (!window) return;
    pulse::ui::Compositor compositor;
    const bool initialized = compositor.Init(window);
    Check(initialized, "initialize actual dialog compositor");
    if (initialized) {
        auto* dc = compositor.Dc();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.2f, 0.5f, 0.8f, 1.0f));
        Check(SUCCEEDED(dc->EndDraw()), "render composition content before fade-in");
        compositor.Present();
        Check(GetPropW(window, L"Pulse.DialogComposition.Visual") != nullptr,
            "composition visual is bound to dialog lifecycle");
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const DWORD backdrop = DWMSBT_MAINWINDOW;
        const bool backdrop_supported = SUCCEEDED(DwmSetWindowAttribute(window,
            DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop))) &&
            SetPropW(window, L"Pulse.DialogBackdrop", reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(backdrop)));
        for (int pass = 0; pass < 2; ++pass) {
            pulse::ui::OwnerDimScope dim(owner);
            pulse::ui::ShowDialogWithFade(window);
            Check(IsWindowVisible(window) && GetWindowLongPtrW(window, GWL_EXSTYLE) == style,
                "composition fade-in preserves no-redirection style without layering");
            Check(!animations || GetPropW(window, L"Pulse.DialogFadeState"),
                "DirectComposition animation starts without waiting for completion");
            if (animations && backdrop_supported)
                Check(pulse::ui::IsDialogSurfaceFading(window),
                    "DWM backdrop follows the content surface during asynchronous fade");
            if (pass == 0) {
                PumpFade(window);
                Check(!pulse::ui::IsDialogSurfaceFading(window),
                    "completed fade restores the DWM backdrop");
            }
            EnableWindow(owner, FALSE);
            const HWND overlay = reinterpret_cast<HWND>(GetPropW(owner, L"Pulse.OwnerDimOverlay"));
            std::atomic_bool stop = false, ready = false;
            Samples closing;
            auto helper = Sample(window, stop, ready, closing, nullptr, overlay);
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            pulse::ui::HideComposedDialog(window, owner);
            stop.store(true, std::memory_order_release);
            helper.join();
            Check(!IsWindowVisible(window) && IsWindow(window) && IsWindowEnabled(owner),
                "composition fade-out hides surface and restores modal owner");
            Check(!pulse::ui::IsDialogSurfaceFading(window),
                "hiding cancels transient backdrop painting");
            Check(!GetPropW(window, L"Pulse.DialogFadeState") && !GetPropW(window, L"Pulse.DialogClosing") &&
                GetPropW(window, L"Pulse.DialogComposition.Visual") &&
                GetWindowLongPtrW(window, GWL_EXSTYLE) == style,
                "composition close clears transient state while retaining compositor binding and window style");
            if (pass == 0 && animations) {
                Check(closing.closing_seen && closing.dim_alpha.size() > 1 &&
                    closing.dim_alpha.front() > closing.dim_alpha.back(),
                    "composition close keeps the owner dim fading through intermediate exit frames");
                if (backdrop_supported) Check(closing.composed_close_seen,
                    "composition exit suppresses backdrop without adding incompatible layered style");
            }
        }
    }
    pulse::ui::ShowDialogWithFade(window, SW_SHOWNOACTIVATE);
    compositor.Shutdown();
    Check(!GetPropW(window, L"Pulse.DialogFadeState") && !pulse::ui::IsDialogSurfaceFading(window),
        "compositor shutdown cancels pending fade before releasing borrowed resources");
    Check(!GetPropW(window, L"Pulse.DialogComposition.Visual"), "shutdown removes borrowed visual binding");
    DestroyWindow(window);
    ShowWindow(owner, SW_HIDE);
}

struct Search { HWND owner = nullptr; HWND dialog = nullptr; };
BOOL CALLBACK FindPrompt(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    wchar_t name[64]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (std::wstring(name) == L"PulseNewItemDialog" &&
        GetWindow(window, GW_OWNER) == search.owner && IsWindowVisible(window)) {
        search.dialog = window;
        return FALSE;
    }
    return TRUE;
}

void CheckPromptStyle(HWND dialog, UINT dpi, bool dark) {
    const auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
    RECT client{}; GetClientRect(dialog, &client);
    const auto info = Inspect(dialog);
    const auto primary = info.primary, cancel = info.cancel, close = info.close;
    Check(close.right - close.left == px(46) && close.bottom == px(44),
        "new-item close target follows 46 by 44 DIP header contract");
    Check(primary.bottom - primary.top == px(32) && cancel.bottom == primary.bottom &&
        primary.left - cancel.right == px(8) && client.right - primary.right == px(20) &&
        client.bottom - primary.bottom == px(16), "new-item actions use right-aligned Cancel then Primary with 64 DIP footer");
    POINT point{px(50), px(10)}; ClientToScreen(dialog, &point);
    Check(SendMessageW(dialog, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCAPTION,
        "new-item semantic header remains draggable");
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(dialog, 100), WM_GETFONT, 0, 0)), sizeof(font), &font);
    Check(font.lfHeight == -px(14), "new-item native input uses 14 DIP text after layout");
    Check(wcscmp(font.lfFaceName, pulse::ui::typography::PreferredTextFamily()) == 0,
        "new-item native input uses the shared text family");
    Check(info.title_font_size == px(14) && info.title_weight == DWRITE_FONT_WEIGHT_BOLD,
        "new-item caption uses the shared bold DirectWrite title format");
    Check(CloseGlyph(dialog), "new-item vector Close is visible at its shared size and centered in its hit target");
    const auto theme = pulse::ui::IsHighContrast() ? pulse::ui::MakeHighContrastTheme() :
        pulse::ui::MakeTheme(dark, D2D1::ColorF(GetRValue(prompt_accent) / 255.0f,
            GetGValue(prompt_accent) / 255.0f, GetBValue(prompt_accent) / 255.0f));
    const auto sheet = pulse::ui::BlendOver(theme.surface_sheet, theme.bg);
    const auto input = pulse::ui::BlendOver(theme.fill_input_focus, sheet);
    const auto outline = pulse::ui::IsHighContrast() ? theme.stroke_card : dark
        ? pulse::ui::HexColor(0xFFFFFF, 0.08f) : pulse::ui::HexColor(0x000000, 13.0f / 255.0f);
    const auto border = pulse::ui::BlendOver(outline, input);
    const auto field = info.fields.front().bounds;
    Check(Pixel(dialog, field.left, field.top + px(8), border),
        "new-item input border composites translucent stroke over the input fill");
    GUITHREADINFO thread{sizeof(thread)};
    GetGUIThreadInfo(GetWindowThreadProcessId(dialog, nullptr), &thread);
    const auto bottom = thread.hwndFocus == GetDlgItem(dialog, 100)
        ? pulse::ui::BlendOver(theme.accent, input) : pulse::ui::BlendOver(theme.stroke_input_bottom, border);
    Check(Pixel(dialog, field.left + px(30), field.bottom - 1, bottom),
        "new-item bottom stroke uses accent only while its native editor owns focus");
}

std::atomic_bool theme_refresh_ran = false;
bool theme_refresh_required = false;
constexpr UINT_PTR kThemeRefreshTimer = 947;
void CALLBACK RefreshThemeFixture(HWND owner, UINT, UINT_PTR, DWORD) {
    Search search{owner};
    EnumThreadWindows(GetCurrentThreadId(), FindPrompt, reinterpret_cast<LPARAM>(&search));
    if (!search.dialog) return;
    KillTimer(owner, kThemeRefreshTimer);
    const HWND dialog = search.dialog;
    const HWND edit = GetDlgItem(dialog, 100);
    if (GetWindowTextLengthW(edit) == 0) SetWindowTextW(edit, L"theme-fixture");
    const bool system_contrast = pulse::ui::IsHighContrast();
    wchar_t text[128]{};
    GetWindowTextW(edit, text, ARRAYSIZE(text));
    {
        pulse::ui::HighContrastFrameScope contrast(true);
        SendMessageW(dialog, WM_SETTINGCHANGE, SPI_SETHIGHCONTRAST, 0);
        HDC dc = GetDC(edit);
        const auto brush = reinterpret_cast<HBRUSH>(SendMessageW(dialog, WM_CTLCOLOREDIT,
            reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(edit)));
        LOGBRUSH info{}; GetObjectW(brush, sizeof(info), &info);
        Check(info.lbColor == GetSysColor(COLOR_WINDOW) && GetTextColor(dc) == GetSysColor(COLOR_WINDOWTEXT),
            "open prompt refreshes input brush and text to the system high-contrast pair");
        ReleaseDC(edit, dc);
    }
    {
        pulse::ui::HighContrastFrameScope contrast(system_contrast);
        SendMessageW(dialog, WM_THEMECHANGED, 0, 0);
    }
    wchar_t retained[128]{}; GetWindowTextW(edit, retained, ARRAYSIZE(retained));
    Check(wcscmp(text, retained) == 0, "theme and contrast refresh preserve entered native edit text");
    theme_refresh_ran = true;
}

void RunPrompt(HWND owner, bool accept) {
    const DWORD ui_thread = GetCurrentThreadId();
    auto helper = std::thread([owner, ui_thread, accept] {
        Search search{owner};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            EnumThreadWindows(ui_thread, FindPrompt, reinterpret_cast<LPARAM>(&search));
            if (search.dialog) {
                if (theme_refresh_required) {
                    const auto theme_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    while (!theme_refresh_ran.load() && std::chrono::steady_clock::now() < theme_deadline)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                Check(GetDlgItem(search.dialog, 100) != nullptr, "new-item prompt contains name edit");
                CheckPromptStyle(search.dialog, pulse::compat::WindowDpi(search.dialog), accept);
                SetWindowTextW(GetDlgItem(search.dialog, 100), L"fade-fixture.txt");
                Check(CaptureFixture(search.dialog, accept ? L"newitem-dark.bmp" : L"newitem-light.bmp"),
                    "save actual new-item dialog client screenshot");
                const UINT dpi = accept ? 144u : 192u;
                RECT rect{}; GetWindowRect(search.dialog, &rect);
                rect.right = rect.left + MulDiv(350, static_cast<int>(dpi), 96);
                rect.bottom = rect.top + MulDiv(224, static_cast<int>(dpi), 96);
                SendMessageW(search.dialog, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
                wchar_t entered[64]{}; GetWindowTextW(GetDlgItem(search.dialog, 100), entered, ARRAYSIZE(entered));
                Check(wcscmp(entered, L"fade-fixture.txt") == 0, "new-item DPI transition preserves entered name");
                CheckPromptStyle(search.dialog, dpi, accept);
                Check(CaptureFixture(search.dialog, accept ? L"newitem-style-144dpi.bmp" : L"newitem-style-192dpi.bmp"),
                    "capture new-item style after DPI transition");
                PostMessageW(search.dialog, WM_COMMAND, accept ? IDOK : IDCANCEL, 0);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(false, "new-item prompt appears on isolated test thread within timeout");
        PostThreadMessageW(ui_thread, WM_QUIT, 1, 0);
    });
    std::wstring result = L"retained-value";
    const bool accepted = pulse::app::PromptNewItemName(owner,
        theme_refresh_required ? L"新建文本文档" : L"Fade test new item",
        accept, prompt_accent, result);
    helper.join();
    Check(accepted == accept, accept ? "new-item accepts after fade" : "new-item cancels after fade");
    Check(result == (accept ? L"fade-fixture.txt" : L"retained-value"),
        accept ? "new-item returns entered output" : "new-item cancellation preserves caller output");
    Check(IsWindowEnabled(owner) != FALSE, "new-item restores isolated owner after dismissal");
}

std::uint64_t ClientHash(HWND window, bool dark) {
    RECT client{};
    GetClientRect(window, &client);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC source = GetDC(window), memory = CreateCompatibleDC(source);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    std::uint64_t hash = 0;
    if (source && memory && bitmap && pixels) {
        const auto previous = SelectObject(memory, bitmap);
        const auto inspection = Inspect(GetParent(window));
        bool custom = false;
        for (const auto& field : inspection.fields) if (field.edit == window) custom = field.custom;
        const bool rendered = custom && inspection.compositor
            ? inspection.compositor->PaintLumaEdit(window, memory, inspection.compositor->TextFormat(),
                pulse::ui::ChildEditColor(pulse::ui::ChildEditTextColor(dark)),
                pulse::ui::ChildEditColor(pulse::ui::ChildEditBackColor(dark)))
            : PrintWindow(window, memory, PW_CLIENTONLY) != FALSE;
        if (rendered) {
            GdiFlush();
            hash = 14695981039346656037ull;
            const auto* bytes = static_cast<const BYTE*>(pixels);
            for (size_t i = 0; i < static_cast<size_t>(client.right) * client.bottom * 4; ++i) {
                if (i % 4 == 3) continue;
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            }
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (source) ReleaseDC(window, source);
    return hash;
}

struct InputCase {
    HWND owner = nullptr;
    bool dark = false, ran = false;
    int parent_paints = 0;
};
InputCase* input_case = nullptr;
constexpr UINT_PTR kInputTimer = 913;

LRESULT CALLBACK ObserveInput(HWND dialog, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR subclass, DWORD_PTR reference) {
    auto& test = *reinterpret_cast<InputCase*>(reference);
    if (message == WM_PAINT) ++test.parent_paints;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(dialog, ObserveInput, subclass);
    return DefSubclassProc(dialog, message, wp, lp);
}

void CALLBACK InputTimer(HWND owner, UINT, UINT_PTR, DWORD) {
    Search search{owner};
    EnumThreadWindows(GetCurrentThreadId(), FindPrompt, reinterpret_cast<LPARAM>(&search));
    if (!search.dialog || (GetWindowLongPtrW(search.dialog, GWL_EXSTYLE) & WS_EX_LAYERED)) return;
    KillTimer(owner, kInputTimer);
    auto& test = *input_case;
    test.ran = true;
    const HWND dialog = search.dialog, edit = GetDlgItem(dialog, 100);
    SetWindowTextW(edit, L"报告");
    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Check(SetWindowSubclass(dialog, ObserveInput, 2, reinterpret_cast<DWORD_PTR>(&test)) != FALSE,
        "observe input redraws on dialog UI thread");
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    for (int i = 0; i < 64; ++i) {
        const LPARAM repeat = 1 | (0x1e << 16) | (1ll << 30);
        SendMessageW(edit, WM_KEYDOWN, 'A', repeat);
        SendMessageW(edit, WM_CHAR, 'a', repeat);
        UpdateWindow(dialog);
        UpdateWindow(edit);
    }
    Check(GetWindowTextLengthW(edit) == 66, "held-key repeat preserves every character");
    Check(test.parent_paints == 0, "valid repeat does not repaint dialog background");
    Check(test.parent_paints == 0, "valid repeat does not repaint unchanged Direct2D actions");
    Check(Inspect(dialog).enabled, "valid repeat leaves create enabled");

    HideCaret(edit);
    double presentation_time = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    Check(SetPropW(edit, L"Pulse.Test.EditTime", reinterpret_cast<HANDLE>(&presentation_time)) != FALSE,
        "hold the input animation clock still across the parent repaint");
    const auto before = ClientHash(edit, test.dark);
    InvalidateRect(dialog, nullptr, FALSE);
    UpdateWindow(dialog);
    const auto after = ClientHash(edit, test.dark);
    if (!before || after != before)
        std::printf("[INFO] input frame hashes before=%llu after=%llu\n",
            static_cast<unsigned long long>(before), static_cast<unsigned long long>(after));
    Check(before != 0 && after == before, "parent repaint preserves rendered native edit pixels at the same animation time");
    RemovePropW(edit, L"Pulse.Test.EditTime");
    ShowCaret(edit);

    const auto local_update = [&](bool error_changed) {
        const auto info = Inspect(dialog);
        RECT client{}; GetClientRect(dialog, &client);
        const auto& primary = info.primary;
        HRGN allowed = CreateRectRgn(static_cast<int>(std::floor(primary.left)), static_cast<int>(std::floor(primary.top)),
            static_cast<int>(std::ceil(primary.right)), static_cast<int>(std::ceil(primary.bottom)));
        if (error_changed) {
            HRGN error = CreateRectRgn(MulDiv(20, pulse::compat::WindowDpi(dialog), 96),
                static_cast<int>(std::floor(info.fields.front().bounds.bottom + 4 * info.scale)),
                client.right - MulDiv(20, pulse::compat::WindowDpi(dialog), 96),
                client.bottom - MulDiv(68, pulse::compat::WindowDpi(dialog), 96));
            CombineRgn(allowed, allowed, error, RGN_OR); DeleteObject(error);
        }
        HRGN update = CreateRectRgn(0, 0, 0, 0);
        const bool pending = GetUpdateRgn(dialog, update, FALSE) > NULLREGION;
        const bool local = CombineRgn(update, update, allowed, RGN_DIFF) == NULLREGION;
        DeleteObject(update); DeleteObject(allowed);
        return pending && local;
    };
    SetWindowTextW(edit, L"bad:name");
    Check(!Inspect(dialog).enabled && !Inspect(dialog).error.empty(), "invalid name disables create and exposes the validation error");
    Check(local_update(true), "invalid transition invalidates only error and primary action regions");
    UpdateWindow(dialog);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    test.parent_paints = 0;
    for (int i = 0; i < 16; ++i) {
        SendMessageW(edit, WM_CHAR, ':', 1);
        UpdateWindow(dialog);
        UpdateWindow(edit);
    }
    Check(test.parent_paints == 0,
        "invalid repeat does not repaint unchanged error or buttons");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    Check(IsWindow(dialog), "invalid name cannot accept dialog");

    SetWindowTextW(edit, L"科技报告");
    Check(Inspect(dialog).enabled && Inspect(dialog).error.empty() && local_update(true),
        "valid transition enables create and clears error locally");
    UpdateWindow(dialog);
    SetWindowTextW(edit, L"");
    Check(!Inspect(dialog).enabled && Inspect(dialog).error.empty() && local_update(false),
        "empty name disables create while invalidating only its action and retaining the empty error area");
    SetWindowTextW(edit, L"科技报告");
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"_粘贴"));
    Check(GetWindowTextLengthW(edit) == 7 && Inspect(dialog).enabled, "Unicode replacement preserves native edit validation");
    SendMessageW(edit, EM_UNDO, 0, 0);
    wchar_t value[32]{};
    GetWindowTextW(edit, value, ARRAYSIZE(value));
    Check(wcscmp(value, L"科技报告") == 0 && Inspect(dialog).enabled, "native undo restores valid Unicode name");
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(IsWindow(dialog) && GetWindowTextLengthW(edit) == 4,
        "synthetic IME composition Enter and Escape preserve the open new-item editor");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    for (const int action : {IDCANCEL, IDOK, 101}) {
        SendMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
        Check(GetFocus() == dialog && Inspect(dialog).focus == action,
            "new-item Tab traverses Cancel, Create and Close in the shared action order");
    }
    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
    Check(GetFocus() == edit, "new-item Tab wraps from Close to its native editor");
    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Check(CaptureFixture(dialog, test.dark ? L"newitem-input-dark.bmp" : L"newitem-input-light.bmp"),
        "capture new-item dialog after repeated input and validation changes");
    RemoveWindowSubclass(dialog, ObserveInput, 2);
    PostMessageW(edit, WM_KEYDOWN, test.dark ? VK_RETURN : VK_ESCAPE, 1);
}

void RunInput(HWND owner, bool dark) {
    InputCase test{owner, dark};
    input_case = &test;
    Check(SetTimer(owner, kInputTimer, 10, InputTimer) != 0, "schedule isolated repeated-input fixture");
    std::wstring result = L"retained-value";
    const bool accepted = pulse::app::PromptNewItemName(owner, L"新建文件夹", dark,
        RGB(80, 180, 220), result);
    KillTimer(owner, kInputTimer);
    input_case = nullptr;
    Check(test.ran, "repeated-input fixture reached real modal dialog");
    Check(accepted == dark && result == (dark ? L"科技报告" : L"retained-value"),
        "Enter accepts and Escape cancels without corrupting caller output");
    Check(IsWindowEnabled(owner), "repeated-input prompt restores modal owner");
}

void RunInputOwnerState(HWND owner) {
    for (const bool initially_enabled : {true, false}) {
        EnableWindow(owner, initially_enabled);
        InputCase test{owner, false};
        input_case = &test;
        Check(SetTimer(owner, kInputTimer, 10, [](HWND timer_owner, UINT, UINT_PTR timer, DWORD) {
            Search search{timer_owner};
            EnumThreadWindows(GetCurrentThreadId(), FindPrompt, reinterpret_cast<LPARAM>(&search));
            if (!search.dialog) return;
            KillTimer(timer_owner, timer);
            input_case->ran = true;
            Check(!IsWindowEnabled(timer_owner), "owner remains disabled while the real new-item prompt is open");
            PostMessageW(GetDlgItem(search.dialog, 100), WM_KEYDOWN, VK_ESCAPE, 1);
        }) != 0, "schedule real input cancellation with the owner's original enabled state");
        std::wstring result = L"retained-owner-state";
        const bool accepted = pulse::app::PromptNewItemName(owner, L"Owner state fixture", false,
            RGB(80, 180, 220), result);
        KillTimer(owner, kInputTimer);
        input_case = nullptr;
        Check(test.ran && !accepted && result == L"retained-owner-state",
            "Escape cancels the real input prompt without changing caller output");
        Check((IsWindowEnabled(owner) != FALSE) == initially_enabled, initially_enabled
            ? "input cancellation reenables an initially enabled owner"
            : "input cancellation preserves an initially disabled owner");
    }
    EnableWindow(owner, TRUE);
}

struct EscapeCase {
    HWND owner = nullptr;
    const wchar_t* dialog_class = nullptr;
    bool owner_should_enable = true;
    bool observed_hide = false;
    bool owner_ready_before_hide = true;
    bool owner_ready_before_destroy = true;
    bool verify_activation = false;
    bool activation_lost = false;
    bool owner_active_before_hide = true;
};
EscapeCase* escape_case = nullptr;
constexpr UINT_PTR kEscapeTimer = 82;

LRESULT CALLBACK ObserveClose(HWND dialog, UINT message, WPARAM wp, LPARAM lp,
                             UINT_PTR subclass, DWORD_PTR reference) {
    auto& test = *reinterpret_cast<EscapeCase*>(reference);
    if (message == WM_WINDOWPOSCHANGING &&
        (reinterpret_cast<WINDOWPOS*>(lp)->flags & SWP_HIDEWINDOW)) {
        test.observed_hide = true;
        test.owner_ready_before_hide &= (IsWindowEnabled(test.owner) != FALSE) == test.owner_should_enable;
        if (test.verify_activation) test.owner_active_before_hide &= GetActiveWindow() == test.owner;
    }
    if (message == WM_DESTROY)
        test.owner_ready_before_destroy &= (IsWindowEnabled(test.owner) != FALSE) == test.owner_should_enable;
    if (message == WM_ACTIVATEAPP && !wp) test.activation_lost = true;
    if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE &&
        reinterpret_cast<HWND>(lp) != test.owner) test.activation_lost = true;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(dialog, ObserveClose, subclass);
    return DefSubclassProc(dialog, message, wp, lp);
}

void CALLBACK EscapeTimer(HWND owner, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM reference) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(candidate, name, ARRAYSIZE(name));
        if (GetWindow(candidate, GW_OWNER) == escape_case->owner && IsWindowVisible(candidate) &&
            wcscmp(name, escape_case->dialog_class) == 0) {
            *reinterpret_cast<HWND*>(reference) = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(owner, kEscapeTimer);
    Check(SetWindowSubclass(dialog, ObserveClose, 1,
        reinterpret_cast<DWORD_PTR>(escape_case)) != FALSE, "observe real modal close on its UI thread");
    escape_case->verify_activation = GetActiveWindow() == dialog && escape_case->owner_should_enable;
    PostMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 1);
}

void RunEscapeCases(HWND owner) {
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    for (bool archive : {false, true}) {
        for (int pass = 0; pass < 3; ++pass) {
            const bool initially_enabled = pass < 2;
            EnableWindow(owner, initially_enabled);
            EscapeCase test{owner, archive ? L"PulseArchiveDialog" : L"PulseNewItemDialog",
                initially_enabled};
            escape_case = &test;
            Check(SetTimer(owner, kEscapeTimer, 10, EscapeTimer) != 0, "schedule ESC in real modal message loop");
            bool accepted = false;
            if (archive) {
                pulse::app::ArchiveExtractOptions options{L"C:\\fade-fixture", L"", false};
                accepted = pulse::app::ShowArchiveExtractDialog(owner, false, RGB(80, 180, 220),
                    L"C:\\fade-fixture\\example.zip", true, options);
                Check(options.destination == L"C:\\fade-fixture", "ESC preserves extraction options");
            } else {
                std::wstring name = L"retained-name";
                accepted = pulse::app::PromptNewItemName(owner, L"ESC new item fixture", true,
                    RGB(80, 180, 220), name);
                Check(name == L"retained-name", "ESC preserves new-item output");
            }
            KillTimer(owner, kEscapeTimer);
            escape_case = nullptr;
            Check(!accepted && test.observed_hide, archive ? "ESC closes real extraction dialog"
                : "ESC closes real new-item dialog");
            Check(test.owner_ready_before_hide && test.owner_ready_before_destroy,
                "owner activation target is restored before modal hiding and destruction");
            Check((IsWindowEnabled(owner) != FALSE) == initially_enabled,
                "repeated ESC preserves original owner enabled state including nested modal case");
            if (test.verify_activation)
                Check(!test.activation_lost && test.owner_active_before_hide,
                    "ESC returns directly to owner without intermediate activation target");
            else if (initially_enabled)
                std::printf("[SKIP] Activation routing: test dialog did not become this thread's active window.\n");
        }
    }
    EnableWindow(owner, TRUE);
    ShowWindow(owner, SW_HIDE);
}
} // namespace

int main(int argc, char** argv) {
    const bool style_only = argc == 2 && std::strcmp(argv[1], "--popup-style-only") == 0;
    const bool input_only = argc == 2 && std::strcmp(argv[1], "--new-item-input") == 0;
    const bool lifecycle_only = argc == 2 && std::strcmp(argv[1], "--lifecycle") == 0;
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW fixture{};
    fixture.lpfnWndProc = FixtureProc;
    fixture.hInstance = GetModuleHandleW(nullptr);
    fixture.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    fixture.lpszClassName = kFixtureClass;
    Check(RegisterClassW(&fixture) != 0, "register native fixture class");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse dialog fade test owner",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 480, nullptr, nullptr, fixture.hInstance, nullptr);
    Check(owner != nullptr, "create isolated modal owner");
    if (!owner) return 1;
    Check(SetWindowSubclass(owner, UiTestProc, 9, 0) != FALSE, "install UI-thread readback dispatch");
    const bool animations = AnimationsEnabled();
    if (style_only) {
        prompt_accent = RGB(0, 120, 212);
        theme_refresh_required = true;
        SetTimer(owner, kThemeRefreshTimer, 1, RefreshThemeFixture);
        RunPrompt(owner, true);
        KillTimer(owner, kThemeRefreshTimer);
        Check(theme_refresh_ran, "UI-thread high-contrast refresh fixture reached the open new-item prompt");
        RunPrompt(owner, false);
    } else if (input_only) {
        RunInput(owner, false);
        RunInput(owner, true);
        RunInputOwnerState(owner);
    } else {
        RunNative(owner, animations);
        RunStyleChanges(owner, animations);
        RunLayered(owner);
        RunComposed(owner, animations);
        if (!lifecycle_only) {
            RunPrompt(owner, true);
            RunPrompt(owner, false);
        }
        RunEscapeCases(owner);
    }
    DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("Dialog fade tests: %d failure(s). BMPs: bench_data/dialog_fade.\n", failures.load());
    std::printf("Input snapshots verify the shared Direct2D frame; native fade fixtures retain client captures.\n");
    if (animations)
        std::printf("[SKIP] Disabled-animation branch: current system animations enabled; settings were not changed.\n");
    return failures.load() == 0 ? 0 : 1;
}
