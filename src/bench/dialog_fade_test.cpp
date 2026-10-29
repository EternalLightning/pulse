#include "../app/new_item_dialog.h"
#include "../app/archive_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/dialog_lifecycle.h"
#include "../ui/ui_compositor.h"
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

namespace {
std::atomic_int failures = 0;
std::atomic_int paints = 0;
int messages_during_fade = 0;
constexpr UINT kResponsivenessProbe = WM_APP + 8;
constexpr wchar_t kFixtureClass[] = L"PulseDialogFadeTestFixture";

void Check(bool condition, const char* description) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) failures.fetch_add(1);
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

void RunPrompt(HWND owner, bool accept) {
    const DWORD ui_thread = GetCurrentThreadId();
    auto helper = std::thread([owner, ui_thread, accept] {
        Search search{owner};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            EnumThreadWindows(ui_thread, FindPrompt, reinterpret_cast<LPARAM>(&search));
            if (search.dialog) {
                Check(GetDlgItem(search.dialog, 100) != nullptr, "new-item prompt contains name edit");
                SetWindowTextW(GetDlgItem(search.dialog, 100), L"fade-fixture.txt");
                Check(CaptureFixture(search.dialog, accept ? L"newitem-dark.bmp" : L"newitem-light.bmp"),
                    "save actual new-item dialog client screenshot");
                PostMessageW(search.dialog, WM_COMMAND, accept ? IDOK : IDCANCEL, 0);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(false, "new-item prompt appears on isolated test thread within timeout");
        PostThreadMessageW(ui_thread, WM_QUIT, 1, 0);
    });
    std::wstring result = L"retained-value";
    const bool accepted = pulse::app::PromptNewItemName(owner, L"Fade test new item",
        accept, RGB(80, 180, 220), result);
    helper.join();
    Check(accepted == accept, accept ? "new-item accepts after fade" : "new-item cancels after fade");
    Check(result == (accept ? L"fade-fixture.txt" : L"retained-value"),
        accept ? "new-item returns entered output" : "new-item cancellation preserves caller output");
    Check(IsWindowEnabled(owner) != FALSE, "new-item restores isolated owner after dismissal");
}

std::uint64_t ClientHash(HWND window) {
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
        if (BitBlt(memory, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY)) {
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
    int parent_paints = 0, button_paints = 0;
};
InputCase* input_case = nullptr;
constexpr UINT_PTR kInputTimer = 913;

LRESULT CALLBACK ObserveInput(HWND dialog, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR subclass, DWORD_PTR reference) {
    auto& test = *reinterpret_cast<InputCase*>(reference);
    if (message == WM_PAINT) ++test.parent_paints;
    if (message == WM_DRAWITEM) ++test.button_paints;
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
    const HWND create = GetDlgItem(dialog, IDOK);
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
    Check(test.button_paints == 0, "valid repeat does not repaint unchanged buttons");
    Check(IsWindowEnabled(create) != FALSE, "valid repeat leaves create enabled");

    HideCaret(edit);
    const auto before = ClientHash(edit);
    InvalidateRect(dialog, nullptr, FALSE);
    UpdateWindow(dialog);
    Check(before != 0 && ClientHash(edit) == before, "parent repaint preserves native edit pixels");
    ShowCaret(edit);

    const auto error_only = [&] {
        RECT update{}, edit_rect{}, overlap{};
        GetWindowRect(edit, &edit_rect);
        MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&edit_rect), 2);
        return GetUpdateRect(dialog, &update, FALSE) && !IntersectRect(&overlap, &update, &edit_rect)
            && update.bottom <= MulDiv(160, pulse::compat::WindowDpi(dialog), 96);
    };
    SetWindowTextW(edit, L"bad:name");
    Check(!IsWindowEnabled(create), "invalid name disables create");
    Check(error_only(), "invalid transition invalidates only error area above footer");
    UpdateWindow(dialog);
    UpdateWindow(create);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    test.parent_paints = test.button_paints = 0;
    for (int i = 0; i < 16; ++i) {
        SendMessageW(edit, WM_CHAR, ':', 1);
        UpdateWindow(dialog);
        UpdateWindow(edit);
    }
    Check(test.parent_paints == 0 && test.button_paints == 0,
        "invalid repeat does not repaint unchanged error or buttons");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    Check(IsWindow(dialog), "invalid name cannot accept dialog");

    SetWindowTextW(edit, L"科技报告");
    Check(IsWindowEnabled(create) && error_only(), "valid transition enables create and clears error locally");
    UpdateWindow(dialog);
    SetWindowTextW(edit, L"");
    Check(!IsWindowEnabled(create) && !GetUpdateRect(dialog, nullptr, FALSE),
        "empty name disables create without repainting error area");
    SetWindowTextW(edit, L"科技报告");
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"_粘贴"));
    Check(GetWindowTextLengthW(edit) == 7 && IsWindowEnabled(create), "Unicode replacement preserves native edit validation");
    SendMessageW(edit, EM_UNDO, 0, 0);
    wchar_t value[32]{};
    GetWindowTextW(edit, value, ARRAYSIZE(value));
    Check(wcscmp(value, L"科技报告") == 0 && IsWindowEnabled(create), "native undo restores valid Unicode name");
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
    const bool animations = AnimationsEnabled();
    if (input_only) {
        RunInput(owner, false);
        RunInput(owner, true);
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
    std::printf("Client screenshots verify GDI content; desktop/compositor appearance requires visual inspection.\n");
    if (animations)
        std::printf("[SKIP] Disabled-animation branch: current system animations enabled; settings were not changed.\n");
    return failures.load() == 0 ? 0 : 1;
}
