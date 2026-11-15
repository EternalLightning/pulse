// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/text_diff_window.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/typography.h"
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
    static int TextRenderingTest() {
        int failures = 0;
        const auto check = [&](bool ok, const char* message) {
            std::cout << (ok ? "[PASS] " : "[FAIL] ") << message << std::endl;
            if (!ok) ++failures;
        };
        const auto previous_mode = typography::CurrentTextRenderMode();
        typography::SetTextRenderMode(typography::TextRenderMode::Auto);
        TextDiffWindow window;
        check(window.EnsureWindow(nullptr), "initialize production document renderer");
        if (!window.hwnd_) {
            typography::SetTextRenderMode(previous_mode);
            return 1;
        }
        const auto folder = std::filesystem::absolute(L"bench_data/text-diff-rendering");
        std::filesystem::create_directories(folder);
        auto left = std::make_shared<diff::DiffSide>();
        auto right = std::make_shared<diff::DiffSide>();
        left->status = right->status = diff::LoadStatus::Ok;
        left->path = L"before.txt"; right->path = L"after.txt";
        for (int i = 0; i < 180; ++i) {
            const auto text = std::to_wstring(i) + L"\t中照复杂汉字 English العربية e\x0301 " + std::wstring(350, L'x');
            left->lines.push_back(text + L" before");
            right->lines.push_back(text + L" after");
        }
        window.left_ = left; window.right_ = right;
        check(diff::ComputeLineDiff(left->lines, right->lines, {}, window.result_),
            "prepare long mixed-script modified lines with real comparison");
        window.RebuildView();
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            window.dark_ = dark; window.scale_ = scale;
            window.compositor_.RecreateTextFormats(scale); window.RecreateFormats();
            SetWindowPos(window.hwnd_, nullptr, 0, 0, static_cast<int>(860 * scale),
                static_cast<int>(600 * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            for (const bool unified : {false, true}) {
                window.unified_ = unified; window.RebuildView(); window.ScrollTo(0);
                window.sel_anchor_ = 1; window.sel_end_ = 3;
                const auto render_ms = [&] {
                    const auto start = std::chrono::steady_clock::now();
                    check(window.Render(), "render real clipped document rows");
                    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                };
                const double first = render_ms();
                const auto* stats = window.compositor_.GetLumaTextStats();
                check(window.compositor_.LumaTextEnabled() && stats, "Auto document check has an active LumaText backend");
                if (!stats) continue;
                auto before = *stats;
                const double cached = render_ms();
                const auto full_draws = stats->draw_calls - before.draw_calls;
                auto left_lines = std::move(left->lines), right_lines = std::move(right->lines);
                left->lines.resize(left_lines.size()); right->lines.resize(right_lines.size());
                before = *stats;
                render_ms();
                const auto empty_draws = stats->draw_calls - before.draw_calls;
                left->lines = std::move(left_lines); right->lines = std::move(right_lines);
                const auto visible = static_cast<uint64_t>(std::ceil((window.body_.bottom - window.body_.top) / window.RowHeight()));
                check(full_draws >= empty_draws + visible * (unified ? 1 : 2),
                    "body rows themselves use LumaText beyond unchanged toolbar and gutter draws");
                window.ScrollTo(window.RowHeight() * 80); window.ScrollX(100 * scale);
                const float scroll_y = window.scroll_y_, scroll_x = window.scroll_x_;
                const double scrolled = render_ms();
                int hit_row = -1;
                check(window.HitTest(window.body_.left + 100 * scale, window.body_.top + 5 * scale,
                        &hit_row) == TextDiffWindow::Hit::Body && hit_row == 80 &&
                    window.sel_anchor_ == 1 && window.sel_end_ == 3 &&
                    window.scroll_x_ == scroll_x && window.scroll_y_ == scroll_y,
                    "mixed-script row rendering preserves selection, horizontal offset and scrolled row hit");
                before = *stats;
                typography::SetTextRenderMode(typography::TextRenderMode::Sharp);
                const double directwrite = render_ms();
                check(stats->draw_calls == before.draw_calls, "Sharp document mode retains DirectWrite");
                typography::SetTextRenderMode(typography::TextRenderMode::Smooth); render_ms();
                check(stats->draw_calls == before.draw_calls, "Smooth document mode retains DirectWrite");
                typography::SetTextRenderMode(typography::TextRenderMode::Auto);
                {
                    HighContrastFrameScope high_contrast(true);
                    render_ms();
                    check(stats->draw_calls == before.draw_calls, "high contrast document mode retains DirectWrite");
                }
                const auto name = std::wstring(unified ? L"unified" : L"split") + (dark ? L"-dark-" : L"-light-") +
                    std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                window.sel_anchor_ = 80; window.sel_end_ = 82;
                check(window.Render((folder / name).c_str()), "capture original-size selected long document viewport");
                std::cout << "[INFO] scale=" << scale << " dark=" << dark << " unified=" << unified
                    << " first_ms=" << first << " cached_ms=" << cached << " scrolled_ms=" << scrolled
                    << " sharp_scrolled_ms=" << directwrite
                    << " body_draws=" << full_draws - empty_draws << std::endl;
            }
        }
        window.Close();
        typography::SetTextRenderMode(previous_mode);
        return failures ? 1 : 0;
    }
    static int ToolbarLayoutTest() {
        int failures = 0;
        const auto check = [&](bool ok, const char* message) {
            std::cout << (ok ? "[PASS] " : "[FAIL] ") << message << std::endl;
            if (!ok) ++failures;
        };
        TextDiffWindow window;
        check(window.EnsureWindow(nullptr), "initialize real diff toolbar without a comparison job");
        if (!window.hwnd_) return 1;
        for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
            l10n::SetLanguage(language);
            for (const bool dark : {false, true}) for (const float scale : {1.0f, 1.5f, 2.0f}) {
                window.dark_ = dark; window.scale_ = scale;
                window.compositor_.RecreateTextFormats(scale); window.RecreateFormats();
                check(window.ui_->GetFontSize() == 14 * scale && window.mono_->GetFontSize() == 13 * scale &&
                    window.gutter_->GetFontSize() == 11.5f * scale && window.small_->GetFontSize() == 11.5f * scale,
                    "toolbar uses shared Body14 while document, gutter and status keep compact formats");
                for (const int width : {860, 1200}) {
                    std::wcout << L"toolbar " << language << L" dark=" << dark << L" scale=" << scale
                        << L" width=" << width << std::endl;
                    SetWindowPos(window.hwnd_, nullptr, 0, 0, static_cast<int>(width * scale),
                        static_cast<int>(600 * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    check(window.Render(), "render real diff toolbar without exporting images");
                    check(ToolbarFits(window), "narrow and wide localized toolbar controls remain inside their rows");
                    const D2D1_RECT_F controls[] = {window.mode_split_, window.mode_unified_, window.only_changes_rc_,
                        window.ignore_ws_rc_, window.ignore_case_rc_, window.swap_rc_};
                    const l10n::StringId ids[] = {l10n::StringId::DiffSideBySide, l10n::StringId::DiffUnified,
                        l10n::StringId::DiffOnlyChanges, l10n::StringId::DiffIgnoreWs,
                        l10n::StringId::DiffIgnoreCase, l10n::StringId::DiffSwap};
                    bool fits = true;
                    for (int i = 0; i < 6; ++i) {
                        const auto& rc = controls[i];
                        const float inset = (i < 2 ? 6.0f : i < 5 ? 28.0f : 40.0f) * scale;
                        const float text_width = window.Measure(l10n::Get(ids[i]), window.ui_.get());
                        fits = fits && rc.bottom - rc.top == 30 * scale &&
                            text_width + inset <= rc.right - rc.left + 0.5f &&
                            (i == 0 || controls[i - 1].right <= rc.left);
                    }
                    check(fits, "localized labels fit shared Body14 content bounds at compact 30 DIP height");
                    const auto swap_width = window.painter_.MeasureButtonWidth(l10n::Get(l10n::StringId::DiffSwap), L"\xE8AB");
                    check(swap_width <= window.swap_rc_.right - window.swap_rc_.left + 0.5f,
                        "toolbar swap bounds fit the actual shared painter measurement");
                }
            }
        }
        window.Close();
        l10n::SetLanguage(L"zh-CN");
        return failures ? 1 : 0;
    }
    static int AppearanceTest() {
        int failures = 0;
        const auto check = [&](bool ok, const char* message) {
            std::cout << (ok ? "[PASS] " : "[FAIL] ") << message << std::endl;
            if (!ok) ++failures;
        };
        const auto folder = std::filesystem::absolute(L"bench_data/text-diff-appearance");
        std::filesystem::create_directories(folder);
        const auto left = folder / L"before.txt", right = folder / L"after.txt";
        for (const auto& path : {left, right}) {
            std::ofstream file(path);
            for (int i = 0; i < 200; ++i) file << i << (path == left ? " before" : " after") << '\n';
        }
        std::filesystem::last_write_time(right, std::filesystem::last_write_time(left) + std::chrono::seconds(5));
        TextDiffWindow window;
        window.Show(nullptr, left.wstring(), right.wstring(), false, HexColor(0x0078d4));
        const auto deadline = GetTickCount64() + 10000;
        while (window.computing_ && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
        }
        check(window.Ready(), "load real diff for appearance preservation");
        if (window.Ready()) {
            window.Render();
            window.ScrollTo(400);
            window.sel_anchor_ = 20; window.sel_end_ = 22;
            const auto generation = window.generation_;
            const auto* retained = window.left_.get();
            const auto rows = window.view_.size();
            const float scroll = window.scroll_y_;
            for (const bool dark : {false, true}) {
                window.SetAppearance(dark, WindowEffect::MicaAlt, HexColor(0x0078d4));
                SendMessageW(window.hwnd_, WM_THEMECHANGED, 0, 0);
                check(window.dark_ == dark && window.effect_ == WindowEffect::MicaAlt &&
                    window.generation_ == generation && window.left_.get() == retained &&
                    window.view_.size() == rows && window.scroll_y_ == scroll &&
                    window.sel_anchor_ == 20 && window.sel_end_ == 22 && !window.computing_,
                    "appearance refresh preserves diff content, selection, scroll and job generation");
                const auto background = BlendOver(WithAlpha(window.accent_, 0.18f),
                    dark ? HexColor(0x202020) : HexColor(0xf3f3f3));
                const auto ink = window.NewerBadgeText(dark ? HexColor(0x202020) : HexColor(0xf3f3f3));
                const float bg_luminance = RelativeLuminance(background), ink_luminance = RelativeLuminance(ink);
                check(((std::max)(bg_luminance, ink_luminance) + 0.05f) /
                    ((std::min)(bg_luminance, ink_luminance) + 0.05f) >= 4.5f,
                    "production newer badge ink exceeds 4.5:1 contrast on composited panel");
                check(window.Render((folder / (dark ? L"badge-dark.png" : L"badge-light.png")).c_str()),
                    "capture newer badge on actual panel background");
            }
            window.SetAppearance(false, WindowEffect::None, HexColor(0xffed00));
            const auto background = BlendOver(WithAlpha(window.accent_, 0.18f), HexColor(0xf3f3f3));
            const auto ink = window.NewerBadgeText(HexColor(0xf3f3f3));
            const float bg_luminance = RelativeLuminance(background), ink_luminance = RelativeLuminance(ink);
            check(((std::max)(bg_luminance, ink_luminance) + 0.05f) /
                ((std::min)(bg_luminance, ink_luminance) + 0.05f) >= 4.5f,
                "bright accent newer badge exceeds 4.5:1 contrast on composited panel");
            check(window.Render((folder / L"badge-bright-light.png").c_str()), "capture bright accent badge");
        }
        window.Close();
        return failures ? 1 : 0;
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
    if (argc == 2 && wcscmp(argv[1], L"--text-rendering") == 0) {
        const int result = Peer::TextRenderingTest();
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--toolbar-layout") == 0) {
        const int result = Peer::ToolbarLayoutTest();
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }
    if (argc == 2 && wcscmp(argv[1], L"--appearance") == 0) {
        const int result = Peer::AppearanceTest();
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
    }
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
