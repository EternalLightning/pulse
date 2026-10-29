#include "../ui/text_diff_window.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <windowsx.h>

namespace pulse::ui {
struct TextDiffWindowTestPeer {
    static bool Done(const TextDiffWindow& window) { return !window.computing_; }
    static bool Ready(const TextDiffWindow& window) { return window.Ready(); }
    static int Modified(const TextDiffWindow& window) { return window.result_.modified; }
    static void Unified(TextDiffWindow& window) {
        window.OnClick((window.mode_unified_.left + window.mode_unified_.right) / 2,
                       window.mode_unified_.top + 5, false);
    }
    static bool ToolbarFits(const TextDiffWindow& window) {
        const auto& a = window.swap_rc_;
        const auto& b = window.position_rc_;
        return a.right <= window.toolbar_.right &&
            (a.right <= b.left || a.bottom <= b.top);
    }
    static bool Snapshot(TextDiffWindow& window, const std::filesystem::path& path) {
        return window.Render(path.c_str());
    }
    static int SmoothScrollTest() {
        int failures = 0;
        auto check = [&](bool ok, const char* name) {
            std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
            if (!ok) ++failures;
        };
        auto pump = [] {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        };
        auto until = [&](const auto& condition) {
            const auto end = GetTickCount64() + 10000;
            do {
                pump();
                if (condition()) return true;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
            } while (GetTickCount64() < end);
            return false;
        };
        BOOL enabled = TRUE;
        HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
        const bool animate = enabled && !(hc.dwFlags & HCF_HIGHCONTRASTON);
        const auto folder = std::filesystem::absolute(L"bench_data/smooth-scroll-diff");
        std::filesystem::create_directories(folder);
        const auto left = folder / L"before.txt", right = folder / L"after.txt";
        for (const auto& path : {left, right}) {
            std::ofstream file(path);
            for (int i = 0; i < 500; ++i) file << i << ' ' << std::string(500, 'x') << '\n';
        }
        TextDiffWindow window;
        window.Show(nullptr, left.wstring(), right.wstring(), false, D2D1::ColorF(0x0078D4));
        const bool ready = until([&] { return !window.computing_ && window.Ready(); });
        check(ready, "smooth diff fixture loads through production comparison");
        if (!ready) return 1;
        window.Render();
        window.ScrollTo(1000.0f);
        const float before = window.scroll_y_;
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        check(window.smooth_scroll_y_.Pending() == animate,
              "vertical wheel respects system animation and high contrast policy");
        if (animate) {
            Sleep(30);
            SendMessageW(window.hwnd_, WM_TIMER, 2, 0);
            check(window.scroll_y_ > before && window.scroll_y_ < before + 3.0f * window.RowHeight(),
                  "vertical wheel exposes a continuous intermediate frame");
        }
        const float reversed = window.scroll_y_;
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        Sleep(30);
        SendMessageW(window.hwnd_, WM_TIMER, 2, 0);
        check(window.scroll_y_ < reversed, "reverse wheel cancels forward momentum");
        check(until([&] { return !window.smooth_scroll_y_.Pending(); }), "vertical animation reaches rest");
        window.ScrollX(-window.scroll_x_);
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, static_cast<WORD>(-WHEEL_DELTA)), 0);
        check(window.smooth_scroll_x_.Pending() == animate, "Shift wheel routes to horizontal animation");
        check(until([&] { return !window.smooth_scroll_x_.Pending(); }) && window.scroll_x_ > 0.0f,
              "Shift wheel advances horizontal offset");
        const float horizontal = window.scroll_x_;
        SendMessageW(window.hwnd_, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        check(until([&] { return !window.smooth_scroll_x_.Pending(); }) && window.scroll_x_ > horizontal,
              "horizontal wheel advances the same continuous pixel model");
        window.ScrollTo(window.ScrollMaximumY());
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        check(window.scroll_y_ == window.ScrollMaximumY() && !window.smooth_scroll_y_.Pending(),
              "bottom boundary stops animation immediately");
        window.ScrollTo(0.0f);
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        check(window.scroll_y_ == 0.0f && !window.smooth_scroll_y_.Pending(),
              "top boundary stops animation immediately");
        window.ScrollTo(1000.0f);
        SendMessageW(window.hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        window.Show(nullptr, right.wstring(), left.wstring(), false, D2D1::ColorF(0x0078D4));
        check(window.scroll_y_ == 0.0f && window.scroll_x_ == 0.0f &&
              !window.smooth_scroll_y_.Pending() && !window.smooth_scroll_x_.Pending(),
              "content switch clears both offsets and pending animation");
        window.Close();
        return failures ? 1 : 0;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    using Peer = ui::TextDiffWindowTestPeer;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && wcscmp(argv[1], L"--smooth-scroll") == 0) {
        const int result = Peer::SmoothScrollTest();
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }
    int failures = 0;
    auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!passed) ++failures;
    };
    const auto folder = std::filesystem::absolute(L"bench_data/text-diff-ui");
    std::filesystem::create_directories(folder);
    const auto left = folder / L"before.txt", right = folder / L"after.txt";
    { std::ofstream file(left); file << "// Pulse text comparison\nname = before\nvalue = 10\nunchanged = true\n"; }
    { std::ofstream file(right); file << "// Pulse text comparison\nname = after\nvalue = 20\nunchanged = true\nadded = yes\n"; }
    auto pump = [] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    };
    auto wait = [&](ui::TextDiffWindow& window) {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (!Peer::Done(window) && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
            pump();
        }
        pump();
        return Peer::Done(window);
    };
    {
        ui::TextDiffWindow window;
        window.Show(nullptr, left.wstring(), right.wstring(), false, D2D1::ColorF(0x0078D4));
        check(window.visible() && wait(window) && Peer::Ready(window) && Peer::Modified(window) == 2,
              "real comparison window loads files and displays modifications");
        check(Peer::ToolbarFits(window) && Peer::Snapshot(window, folder / L"split-light.png"),
              "capture light split diff with nonoverlapping toolbar");
        Peer::Unified(window);
        check(Peer::Snapshot(window, folder / L"unified-light.png"), "click unified control and capture result");
        const auto start = std::chrono::steady_clock::now();
        window.Show(nullptr, right.wstring(), left.wstring(), true, D2D1::ColorF(0x0078D4));
        window.Close();
        check(!window.visible() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500),
              "retarget and close do not wait for worker completion");
        window.Show(nullptr, left.wstring(), right.wstring(), true, D2D1::ColorF(0x0078D4));
        check(wait(window) && Peer::Ready(window), "reopening only adopts current generation");
        const UINT dpi = compat::WindowDpi(window.hwnd());
        SetWindowPos(window.hwnd(), nullptr, 0, 0, MulDiv(860, dpi, 96), MulDiv(600, dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER);
        pump();
        check(Peer::Snapshot(window, folder / L"narrow-dark.png") && Peer::ToolbarFits(window),
              "capture narrow dark diff with wrapping toolbar");
        window.Show(nullptr, left.wstring(), (folder / L"missing.txt").wstring(), true, D2D1::ColorF(0x0078D4));
        check(wait(window) && !Peer::Ready(window) && Peer::Snapshot(window, folder / L"read-error-dark.png"),
              "deleted or missing source produces a rendered error");
        SendMessageW(window.hwnd(), WM_CLOSE, 0, 0);
        check(!window.visible(), "native close hides comparison and cancels its job");
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return failures ? 1 : 0;
}
