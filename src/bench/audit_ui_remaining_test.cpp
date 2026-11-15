// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/advanced_search_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/connect_server_dialog.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/text_diff_window.h"
#include "../ui/color_picker.h"
#include "../ui/fluent_menu.h"
#include "../ui/typography.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
int failures = 0;
int dpi = 96;
UINT_PTR drive_timer = 0;
std::wstring active_class;
HWND fixture_owner = nullptr;
bool batch = false;
bool dialog_buttons_only = false;
bool dialog_titles_only = false;
bool color_picker_only = false;
bool dialog_dark = false;
bool submit_batch = false;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
std::filesystem::path output;
bool Snapshot(HWND window, const std::wstring& name) {
    const auto path = output / (name + (dialog_buttons_only ? dialog_dark ? L"-dark" : L"-light" : L"") +
        L"-" + std::to_wstring(dpi) + L".png");
    return SendMessageW(window, WM_APP + 0x2B5, 0, reinterpret_cast<LPARAM>(path.c_str())) != 0;
}
struct OwnedWindowSearch {
    HWND owner;
    const wchar_t* class_name;
    DWORD process_id;
    DWORD thread_id;
    HWND result = nullptr;
};
bool MatchesOwnedWindow(HWND window, const OwnedWindowSearch& search) {
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
    if (!window || !IsWindowVisible(window) || process_id != search.process_id ||
        thread_id != search.thread_id || GetWindow(window, GW_OWNER) != search.owner) return false;
    wchar_t name[128]{};
    return GetClassNameW(window, name, ARRAYSIZE(name)) > 0 &&
        std::wstring_view(name) == search.class_name;
}
BOOL CALLBACK FindOwnedWindow(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<OwnedWindowSearch*>(data);
    if (MatchesOwnedWindow(window, search)) { search.result = window; return FALSE; }
    return TRUE;
}
BOOL CALLBACK Edits(HWND window, LPARAM data) {
    wchar_t name[32]{}; GetClassNameW(window, name, ARRAYSIZE(name));
    if (_wcsicmp(name, L"Edit") == 0) reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
    return TRUE;
}
void Tab() {
    SendMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
}
void ShiftTab() {
    BYTE keys[256]{}; GetKeyboardState(keys); const BYTE saved = keys[VK_SHIFT];
    keys[VK_SHIFT] = 0x80; SetKeyboardState(keys); Tab(); keys[VK_SHIFT] = saved; SetKeyboardState(keys);
}
LPARAM PrimaryPoint() {
    OwnedWindowSearch search{fixture_owner, active_class.c_str(), GetCurrentProcessId(), GetCurrentThreadId()};
    EnumThreadWindows(search.thread_id, FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    RECT client{};
    GetClientRect(search.result, &client);
    return MAKELPARAM(client.right - 64 * dpi / 96, client.bottom - 32 * dpi / 96);
}
void PrimaryStates(HWND dialog, HWND edit, const std::wstring& name) {
    SetFocus(edit);
    SendMessageW(dialog, WM_MOUSELEAVE, 0, 0);
    Check(Snapshot(dialog, name + L"-normal"), "primary normal screenshot saved");
    SendMessageW(dialog, WM_MOUSEMOVE, 0, PrimaryPoint());
    Check(Snapshot(dialog, name + L"-hover"), "primary hover screenshot saved");
    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, PrimaryPoint());
    Check(GetCapture() == dialog, "primary press captures the pointer");
    Check(Snapshot(dialog, name + L"-pressed"), "primary pressed screenshot saved");
    SendMessageW(dialog, WM_LBUTTONUP, 0, MAKELPARAM(0, 0));
    Check(IsWindow(dialog), "releasing outside the primary button leaves the dialog open");
    SendMessageW(dialog, WM_MOUSELEAVE, 0, 0);
    SetFocus(edit);
    ShiftTab();
    Check(Snapshot(dialog, name + L"-close-focus"), "close keyboard focus screenshot saved");
    ShiftTab();
    Check(Snapshot(dialog, name + L"-focus"), "primary keyboard focus screenshot saved");
    ShiftTab();
    Check(Snapshot(dialog, name + L"-cancel-focus"), "cancel keyboard focus screenshot saved");
    SetFocus(edit);
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    OwnedWindowSearch search{fixture_owner, active_class.c_str(), GetCurrentProcessId(), GetCurrentThreadId()};
    EnumThreadWindows(search.thread_id, FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    const HWND dialog = search.result;
    if (!MatchesOwnedWindow(dialog, search)) return;
    KillTimer(nullptr, drive_timer);
    RECT bounds{}; GetWindowRect(dialog, &bounds);
    const LONG width = active_class == L"PulseConnectServerWindow" ? 520 : batch ? 620 : 560;
    const bool conflict = active_class == L"PulseFileConflictWindow";
    const LONG height = active_class == L"PulseConnectServerWindow" ? 380 : batch ? 588 : conflict ? 428 : 516;
    bounds.right = bounds.left + (conflict ? 480 : width) * dpi / 96;
    bounds.bottom = bounds.top + height * dpi / 96;
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
    if (dialog_titles_only) {
        Check(Snapshot(dialog, batch ? L"batch-title" : active_class == L"PulseConnectServerWindow"
            ? L"server-title" : L"advanced-title"), "capture actual modal title at the requested DPI and theme");
        SendMessageW(dialog, WM_CLOSE, 0, 0);
        return;
    }
    if (conflict) {
        MINMAXINFO minimum{}; SendMessageW(dialog, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
        Check(minimum.ptMinTrackSize.x == 480 * dpi / 96 && minimum.ptMinTrackSize.y == 428 * dpi / 96,
            "collapsed conflict minimum follows DPI");
        for (int i = 0; i < 4; ++i) SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(dialog, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
        Check(minimum.ptMinTrackSize.y == 538 * dpi / 96,
            "keyboard expansion increases the conflict tracking minimum");
        GetWindowRect(dialog, &bounds);
        Check(bounds.bottom - bounds.top == 538 * dpi / 96,
            "expanded conflict window adopts its full content height");
        SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(dialog, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
        Check(minimum.ptMinTrackSize.y == 428 * dpi / 96,
            "collapsing details restores the conflict minimum");
        SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        return;
    }
    const HWND first = GetFocus();
    Check(first && first != dialog && IsChild(dialog, first), "dialog starts in a real native edit control");
    if (dialog_buttons_only) {
        if (submit_batch) {
            Tab(); Tab(); Tab();
            const HWND pattern = GetFocus();
            Check(pattern && pattern != dialog && IsChild(dialog, pattern), "batch pattern edit receives focus");
            SetWindowTextW(pattern, L"renamed-{name}{ext}");
            const auto deadline = GetTickCount64() + 5000;
            while (IsWindow(dialog) && GetTickCount64() < deadline) {
                SendMessageW(dialog, WM_TIMER, 1, 0);
                PrimaryStates(dialog, first, L"batch-primary");
                SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, PrimaryPoint());
                SendMessageW(dialog, WM_LBUTTONUP, 0, PrimaryPoint());
                if (!IsWindow(dialog)) break;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            Check(!IsWindow(dialog), "batch primary accepts after its asynchronous preview becomes ready");
            if (IsWindow(dialog)) SendMessageW(dialog, WM_CLOSE, 0, 0);
            return;
        }
        if (!batch) SetWindowTextW(first, L"");
        Check(Snapshot(dialog, batch ? L"batch-primary-disabled" : L"server-primary-disabled"),
            "disabled primary screenshot saved");
        SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, PrimaryPoint());
        SendMessageW(dialog, WM_LBUTTONUP, 0, PrimaryPoint());
        Check(IsWindow(dialog), "disabled primary click leaves the dialog open");
        if (!batch) {
            SetWindowTextW(first, L"\\\\fixture\\share");
            PrimaryStates(dialog, first, L"server-primary");
        }
    }
    if (active_class == L"PulseConnectServerWindow") {
        MINMAXINFO minimum{}; SendMessageW(dialog, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
        Check(minimum.ptMinTrackSize.x == width * dpi / 96 && minimum.ptMinTrackSize.y == height * dpi / 96,
            "server minimum tracking size follows DPI");
        Tab(); Check(GetFocus() == dialog, "Tab reaches server pin checkbox");
        ShiftTab(); Check(GetFocus() == first, "Shift+Tab returns from pin to the address edit");
        Tab(); SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        Check(Snapshot(dialog, L"server-pin-focus"), "server focus frame screenshot saved");
        Tab(); Tab(); SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    } else if (batch) {
        MINMAXINFO minimum{}; SendMessageW(dialog, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
        Check(minimum.ptMinTrackSize.x == width * dpi / 96 && minimum.ptMinTrackSize.y == height * dpi / 96,
            "batch rename minimum tracking size follows DPI");
        Tab(); Check(GetFocus() != first && GetFocus() != dialog, "batch Tab moves to replacement edit");
        Tab(); Check(GetFocus() == dialog, "batch Tab reaches include-extension checkbox");
        SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        Check(Snapshot(dialog, L"batch-checkbox-focus"), "batch checkbox focus screenshot saved");
        Tab(); const HWND pattern = GetFocus(); Tab();
        Check(GetFocus() == dialog, "batch Tab reaches pattern shortcut");
        SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        wchar_t text[128]{}; GetWindowTextW(pattern, text, ARRAYSIZE(text));
        Check(std::wstring_view(text).find(L"{n}") != std::wstring_view::npos,
            "keyboard shortcut activation changes the real pattern edit");
        if (dialog_buttons_only) {
            Tab(); Tab();
            Check(Snapshot(dialog, L"batch-chip-focus"), "batch shortcut keyboard focus screenshot saved");
            ShiftTab(); ShiftTab();
        }
        Tab(); Check(GetFocus() != dialog && GetFocus() != pattern, "index field enters the focus order when enabled");
        SendMessageW(dialog, WM_CLOSE, 0, 0);
    } else {
        Tab(); Check(GetFocus() == dialog, "advanced-search Tab reaches name-mode dropdown");
        ShiftTab(); Check(GetFocus() == first, "advanced-search Shift+Tab returns to name edit");
        for (int i = 0; i < 11; ++i) Tab();
        SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        Tab(); SendMessageW(dialog, WM_KEYDOWN, VK_SPACE, 0);
        Check(Snapshot(dialog, L"advanced-checkbox-focus"), "advanced-search checkbox focus screenshot saved");
        for (int i = 0; i < 3; ++i) Tab();
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    }
}
void Write(const std::filesystem::path& path, const char* text) {
    FILE* stream = nullptr; _wfopen_s(&stream, path.c_str(), L"wb");
    if (stream) { std::fputs(text, stream); std::fclose(stream); }
}
}

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static void Text(ThumbnailCache& cache, const std::wstring& path, uint32_t pixels, bool hex) {
        const auto key = cache.Key(path, pixels, 0, 0);
        ThumbnailCache::Item item;
        item.kind = hex ? ipc::PreviewContentKind::Hex : ipc::PreviewContentKind::Text;
        for (int i = 0; i < 300; ++i) item.text += L"long text or hex fixture line 0123456789\r\n";
        cache.lru_.push_front(key); item.lru_position = cache.lru_.begin();
        cache.items_.insert_or_assign(key, std::move(item));
    }
};
struct QuickPreviewPlaybackProbe {
    static uint64_t StylePixels(QuickPreviewWindow& window, bool playback, D2D1_RECT_F area) {
        auto* dc = window.compositor_.Dc();
        ComPtr<ID2D1Image> previous;
        ComPtr<ID2D1Bitmap1> target, cpu;
        dc->GetTarget(&previous);
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const D2D1_SIZE_U size{static_cast<UINT32>(window.compositor_.Width()),
            static_cast<UINT32>(window.compositor_.Height())};
        bool ok = SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, properties, &target));
        if (!ok) { Check(false, "create preview style render target"); return 0; }
        dc->SetTarget(target.get());
        dc->BeginDraw();
        const auto theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(window.dark_, window.accent_);
        dc->Clear(window.dark_ ? HexColor(0x151515) : HexColor(0xF7F7F7));
        ComPtr<ID2D1SolidColorBrush> ink;
        dc->CreateSolidColorBrush(theme.text, &ink);
        if (playback) window.DrawPlayback(dc, ink.get());
        else window.DrawTextPreview(dc, ink.get());
        ok = SUCCEEDED(dc->EndDraw());
        dc->SetTarget(previous.get());
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ok = ok && SUCCEEDED(dc->CreateBitmap(size, nullptr, 0, properties, &cpu)) &&
            SUCCEEDED(cpu->CopyFromBitmap(nullptr, target.get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        ok = ok && SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        Check(ok, "preview style renders and reads back without exporting images");
        if (!ok) return 0;
        uint64_t hash = 14695981039346656037ull;
        for (int y = (std::max)(0, static_cast<int>(area.top));
             y < (std::min)(static_cast<int>(size.height), static_cast<int>(area.bottom)); ++y) {
            for (int x = (std::max)(0, static_cast<int>(area.left));
                 x < (std::min)(static_cast<int>(size.width), static_cast<int>(area.right)); ++x) {
                hash ^= reinterpret_cast<const uint32_t*>(mapped.bits + y * mapped.pitch)[x];
                hash *= 1099511628211ull;
            }
        }
        cpu->Unmap();
        return hash;
    }
    static void Style(HWND owner) {
        QuickPreviewWindow window;
        Check(window.Initialize(owner, WM_APP + 1, WM_APP + 2), "preview style fixture initializes");
        if (!window.hwnd_) return;
        ShowWindow(window.hwnd_, SW_SHOW);
        SetFocus(window.hwnd_);
        Check(GetFocus() == window.hwnd_, "preview style fixture owns real keyboard focus");
        for (const bool high_contrast : {false, true}) {
            HighContrastFrameScope contrast(high_contrast);
            for (const bool dark : {false, true}) for (const float scale : {1.0f, 1.5f, 2.0f}) {
                window.dark_ = dark; window.scale_ = scale;
                window.RecreateFormats(); window.compositor_.RecreateTextFormats(scale);
                window.compositor_.Resize(static_cast<int>(480 * scale), static_cast<int>(320 * scale));
                window.frame_count_ = 4;
                window.animation_active_ = false;
                const auto button = window.PlaybackButtonRect(1);
                const LPARAM point = MAKELPARAM(static_cast<int>((button.left + button.right) / 2),
                    static_cast<int>((button.top + button.bottom) / 2));
                const auto area = D2D1::RectF(button.left - 4 * scale, button.top - 4 * scale,
                    button.right + 4 * scale, button.bottom + 4 * scale);
                SendMessageW(window.hwnd_, WM_MOUSELEAVE, 0, 0);
                const auto resting = StylePixels(window, true, area);
                SendMessageW(window.hwnd_, WM_MOUSEMOVE, 0, point);
                Check(window.playback_hover_ == 1 && StylePixels(window, true, area) != resting,
                    "preview playback hover changes real shared button pixels");
                SendMessageW(window.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
                Check(window.playback_pressed_ == 1 && GetCapture() == window.hwnd_ && window.animation_active_,
                    "preview button press captures mouse and preserves immediate playback action");
                const auto pressed = StylePixels(window, true, area);
                SendMessageW(window.hwnd_, WM_LBUTTONUP, 0, point);
                Check(window.playback_pressed_ == -1 && GetCapture() != window.hwnd_ &&
                    window.animation_active_ && (high_contrast || StylePixels(window, true, area) != pressed),
                    "preview mouse release clears shared pressed style without toggling twice");
                SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
                Check(window.playback_keyboard_focus_ == 1 && !window.animation_active_,
                    "existing playback shortcut selects the matching shared focus indicator");
                SendMessageW(window.hwnd_, WM_KEYUP, VK_SPACE, 0);
                const auto focused = StylePixels(window, true, area);
                SendMessageW(window.hwnd_, WM_KILLFOCUS, reinterpret_cast<WPARAM>(owner), 0);
                Check(window.playback_pressed_ == -1 && window.playback_keyboard_focus_ == -1 &&
                    StylePixels(window, true, area) != focused, "preview shared focus ring clears on focus loss");
                SendMessageW(window.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(window.hwnd_, WM_CAPTURECHANGED, 0, 0);
                Check(window.playback_pressed_ == -1, "preview capture loss clears the button pressed state");
                ReleaseCapture();
                window.ResetPlayback(); window.ResetAnimation();
                window.EnsureTextLayout(L"MMMM selected preview text", false, 420 * scale);
                window.sel_anchor_ = 0; window.sel_focus_ = 13;
                window.accent_ = HexColor(0x0078D4);
                const auto text_area = window.TextViewport();
                const auto blue = StylePixels(window, false, text_area);
                window.accent_ = HexColor(0xD00090);
                const auto magenta = StylePixels(window, false, text_area);
                Check(high_contrast ? blue == magenta : blue != magenta,
                    "selection pixels follow the accent while high contrast keeps its readable system outline");
                window.ResetTextState();
            }
        }
        for (const wchar_t* language : {L"zh-CN", L"en-US"}) for (const float scale : {1.0f, 1.5f, 2.0f}) {
            l10n::SetLanguage(language);
            window.scale_ = scale;
            window.compositor_.RecreateTextFormats(scale); window.RecreateFormats();
            window.compositor_.Resize(static_cast<int>(480 * scale), static_cast<int>(320 * scale));
            window.frame_count_ = 4;
            const l10n::StringId ids[] = {l10n::StringId::PlaybackPrevFrame,
                l10n::StringId::PlaybackPlay, l10n::StringId::PlaybackNextFrame};
            bool fits = true;
            for (int i = 0; i < 3; ++i) {
                const auto rc = window.PlaybackButtonRect(i);
                fits = fits && window.painter_.MeasureButtonWidth(l10n::Get(ids[i])) <= rc.right - rc.left &&
                    rc.bottom - rc.top == 32 * scale && window.HitPlaybackButton(
                        {static_cast<LONG>((rc.left + rc.right) / 2), static_cast<LONG>((rc.top + rc.bottom) / 2)}) == i;
            }
            const auto track = window.PlaybackTrackRect();
            Check(fits && track.left > window.PlaybackButtonRect(2).right && track.right - track.left > 80 * scale &&
                track.right < window.PlaybackRect().right,
                "localized Body14 playback labels fit draw and hit bounds with a draggable timeline at minimum width");
        }
        l10n::SetLanguage(L"zh-CN");
        window.Close();
    }
    static void Run(HWND owner) {
        QuickPreviewWindow window;
        auto cache = std::make_shared<ThumbnailCache>();
        Check(window.Initialize(owner, WM_APP + 1, WM_APP + 2, 0, cache), "quick-preview initializes its real render window");
        window.safe_mode_ = true;
        window.item_.path = L"C:\\Pulse-audit-controlled-text.txt";
        window.item_.name = L"controlled long text";
        const auto pixels = window.RequestedPixelSize(window.ContentRect());
        ThumbnailCacheTestAccess::Text(*cache, window.item_.path, pixels, false);
        ShowWindow(window.hwnd_, SW_SHOW); window.Render();
        Check(window.TextScrollMaximum() > 0, "quick-preview renders long native text from the controlled cache");
        window.text_smooth_scroll_.ScrollBy(window.text_scroll_, 120, window.TextScrollMaximum(), 500,
            GetTickCount64(), true);
        window.UpdateTextScrollTimer(); window.Render();
        Check(window.text_smooth_scroll_.Pending(), "ordinary Render retains queued animated scroll");
        Sleep(25); window.Render();
        Check(window.text_scroll_ > 0 && window.text_smooth_scroll_.Pending(), "rendered text advances while keeping the remaining motion");
        window.text_smooth_scroll_.ScrollBy(window.text_scroll_, -60, window.TextScrollMaximum(), 500,
            GetTickCount64(), true);
        window.Render(); Check(window.text_smooth_scroll_.Pending(), "reverse wheel request survives repaint");
        SetWindowPos(window.hwnd_, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER);
        Check(!window.text_smooth_scroll_.Pending(), "a real viewport resize resets obsolete motion");
        window.Render(); Check(window.text_scroll_ <= window.TextScrollMaximum(), "resize clamps the text offset");
        RECT client{}; GetClientRect(window.hwnd_, &client);
        Check(window.compositor_.Width() == client.right &&
            window.compositor_.Height() == client.bottom,
            "quick-preview compositor follows the actual resized viewport");
        window.Close();
    }
};
struct FileOperationFailureProbe {
    static void Run(HWND owner) {
        int pauses = 0, resumes = 0, cancels = 0;
        FileOperationWindow window;
        window.Create(owner, {[&] { ++cancels; }, [&] { ++pauses; }, [&] { ++resumes; }, [] {}});
        ops::OpStatus status; status.active = true; status.phase = ops::OpPhase::Running;
        status.type = ops::OpType::Copy; status.total_items = 10;
        window.Update(status); window.Show(); SetFocus(window.hwnd_);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(window.detailed_, "keyboard details activation expands the operation window");
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_RETURN, 0);
        Check(pauses == 1, "keyboard pause reaches the production callback");
        status.phase = ops::OpPhase::Paused; window.Update(status);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(resumes == 1, "keyboard resume reaches the production callback");
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        window.Render(false);
        Check(window.compositor_.SaveSnapshot((output / L"operation-cancel-focus.png").c_str()),
            "operation compositor focus screenshot saved");
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(cancels == 1, "keyboard cancel reaches the production callback");
        window.Destroy();
    }
};
struct TextDiffWindowTestPeer {
    static bool WaitReady(TextDiffWindow& window) {
        const auto deadline = GetTickCount64() + 5000;
        while (!window.Ready() && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
        return window.Ready();
    }
    static void Run(HWND owner, const std::filesystem::path& left, const std::filesystem::path& right) {
        TextDiffWindow window; window.Show(owner, left.wstring(), right.wstring(), false, HexColor(0x3574BA));
        Check(WaitReady(window), "diff fixture receives the real completed comparison");
        UpdateWindow(window.hwnd_);
        SetFocus(window.hwnd_);
        for (int i = 0; i < 3; ++i) SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(window.only_changes_, "diff keyboard focus activates only-changes mode");
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(window.options_.ignore_whitespace, "diff keyboard focus activates ignore-whitespace");
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(window.hwnd_, WM_KEYDOWN, VK_SPACE, 0);
        Check(window.options_.ignore_case, "diff keyboard focus activates ignore-case");
        Check(WaitReady(window), "diff recomparison completes before its focus screenshot");
        SetFocus(window.hwnd_);
        Check(GetFocus() == window.hwnd_ && window.keyboard_focus_ == TextDiffWindow::Hit::IgnoreCase,
            "diff snapshot has real focus on the ignore-case option");
        Check(window.Render((output / L"diff-options-focus.png").c_str()), "diff compositor focus screenshot saved");
        window.Close();
    }
};
struct FluentMenuTestPeer {
    static void Run(HWND owner, Compositor& compositor) {
        FluentMenu menu; menu.Create(owner, &compositor, 1);
        menu.filter_fn_ = [](const auto&) { return std::vector<FluentMenuItem>{}; };
        menu.EnsureFilterEdit(); LOGFONTW first{}; GetObjectW(menu.edit_font_, sizeof(first), &first);
        menu.SetScale(1.5f); menu.EnsureFilterEdit();
        LOGFONTW scaled{}; GetObjectW(menu.edit_font_, sizeof(scaled), &scaled);
        Check(first.lfHeight == -14 && scaled.lfHeight == -21,
            "reused filter edit font is rebuilt at the new menu scale");
        menu.SetScale(1); menu.EnsureFilterEdit();
        GetObjectW(menu.edit_font_, sizeof(scaled), &scaled);
        Check(scaled.lfHeight == -14, "menu scale can return to the original DPI");
        menu.filter_fn_ = nullptr;
        const auto path = output / L"menu-focus.png";
        Check(menu.SaveDebugSnapshot(path.c_str(), {{1, L"Selected menu item", L"", L"Ctrl+K"},
            {2, L"Another item"}}, 0), "real menu renders a reviewable snapshot");
    }
    static void HighContrast(Compositor& compositor) {
        const Theme system = MakeHighContrastTheme();
        const COLORREF selected = GetSysColor(COLOR_HIGHLIGHTTEXT);
        Check(std::abs(system.text_selected.r - GetRValue(selected) / 255.0f) < 0.001f &&
            std::abs(system.text_selected.g - GetGValue(selected) / 255.0f) < 0.001f &&
            std::abs(system.text_selected.b - GetBValue(selected) / 255.0f) < 0.001f,
            "high-contrast selected text uses the system highlight foreground");
        Theme controlled = system;
        controlled.text = HexColor(0xFF0000);
        controlled.text_selected = HexColor(0x00FF00);
        controlled.text_disabled = HexColor(0x808080);
        controlled.fill_hover = controlled.fill_pressed = HexColor(0x000060);
        compositor.Resize(520, 180);
        auto* dc = compositor.Dc();
        fluent::Painter painter; painter.SetCompositor(&compositor); painter.SetScale(1);
        dc->BeginDraw(); dc->Clear(HexColor(0x000000));
        painter.BeginFrame(controlled, true);
        for (int row = 0; row < 4; ++row) {
            fluent::MenuItemSpec item;
            item.bounds = D2D1::RectF(0, row * 40.0f, 520, row * 40.0f + 40);
            item.text = L"Controlled menu foreground"; item.shortcut = L"Ctrl+K";
            item.glyph = L"\xE8A7";
            item.state.hovered = row == 1; item.state.selected = row == 2;
            item.state.enabled = row != 3;
            painter.DrawMenuItem(item);
        }
        Check(SUCCEEDED(dc->EndDraw()) && compositor.SaveSnapshot((output / L"menu-high-contrast-controlled.png").c_str()),
            "controlled high-contrast menu renders without changing system preferences");
        Microsoft::WRL::ComPtr<ID2D1Image> image;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> target, readable;
        dc->GetTarget(&image);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        bool mapped_ok = image && SUCCEEDED(image.As(&target)) &&
            SUCCEEDED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, properties, &readable)) &&
            SUCCEEDED(readable->CopyFromBitmap(nullptr, target.Get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        mapped_ok = mapped_ok && SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        int green[4]{}, red[4]{}, disabled_gray = 0, shortcut_green = 0, icon_green = 0;
        if (mapped_ok) {
            for (int row = 0; row < 4; ++row) for (int y = row * 40; y < (row + 1) * 40; ++y) {
                for (int x = 20; x < 500; ++x) {
                    const auto* pixel = mapped.bits + y * mapped.pitch + x * 4;
                    if (pixel[1] > 160 && pixel[0] < 80 && pixel[2] < 80) {
                        ++green[row]; if (row == 1 || row == 2) {
                            if (x > 430) ++shortcut_green;
                            if (x < 40) ++icon_green;
                        }
                    }
                    if (pixel[2] > 160 && pixel[0] < 80 && pixel[1] < 80) ++red[row];
                    if (row == 3 && pixel[0] > 100 && pixel[0] < 150 &&
                        std::abs(pixel[0] - pixel[1]) < 3 && std::abs(pixel[1] - pixel[2]) < 3) ++disabled_gray;
                }
            }
            readable->Unmap();
        }
        Check(mapped_ok && green[1] > 30 && green[2] > 30 && red[1] == 0 && red[2] == 0 &&
            shortcut_green > 10 && icon_green > 2,
            "hover and selected labels, icons, and shortcuts render the paired foreground");
        Check(mapped_ok && red[0] > 30 && green[0] == 0 && disabled_gray > 30 && green[3] == 0 && red[3] == 0,
            "ordinary and disabled menu rows retain their distinct high-contrast colors");
    }
};
}

namespace {
void CheckPickerTextAlignment(const std::filesystem::path& path, bool dark, float scale) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    bool loaded = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom));
    UINT width = 0, height = 0;
    loaded = loaded && SUCCEEDED(converter->GetSize(&width, &height));
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    loaded = loaded && SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    Check(loaded, "read actual picker pixels for text alignment");
    if (!loaded) return;
    const float shadow = static_cast<float>(pulse::ui::FluentMenu::kShadowMargin);
    const auto ink_center = [&](float left, float top, float right, float bottom, const char* label) {
        const int x0 = std::max(0, static_cast<int>(std::ceil(shadow + left * scale)));
        const int y0 = std::max(0, static_cast<int>(std::ceil(shadow + top * scale)));
        const int x1 = std::min(static_cast<int>(width), static_cast<int>(std::floor(shadow + right * scale)));
        const int y1 = std::min(static_cast<int>(height), static_cast<int>(std::floor(shadow + bottom * scale)));
        int first = y1, last = y0 - 1;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            const auto* pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            const bool ink = dark ? std::min({pixel[0], pixel[1], pixel[2]}) > 180
                                  : std::max({pixel[0], pixel[1], pixel[2]}) < 120;
            if (pixel[3] > 180 && ink) { first = std::min(first, y); last = std::max(last, y); }
        }
        const float center = (first + last + 1) * 0.5f;
        const float expected = shadow + (top + bottom) * 0.5f * scale;
        const bool centered = last >= first && std::abs(center - expected) <= 2.5f * scale;
        if (!centered) std::printf("[ALIGN] %s scale=%.1f dark=%d ink=%d..%d expected=%.2f\n",
            label, scale, dark, first, last, expected);
        Check(centered, label);
        return center;
    };
    ink_center(30, 315, 76, 349, "RGB/HSV text is vertically centered inside its control");
    ink_center(124, 315, 262, 349, "hex value is vertically centered inside its control");
    for (int row = 0; row < 4; ++row) {
        const float top = 361.0f + row * 46.0f, bottom = top + 34.0f;
        const auto label = ink_center(18, top, 30, bottom, "channel label is vertically centered in its row");
        const auto value = ink_center(52, top, 106, bottom, "channel number is vertically centered in its input");
        Check(std::abs(label - value) <= 1.5f * scale, "channel label and input value share the same baseline");
    }
    const auto icon_size = [&](float center_x) {
        int left = static_cast<int>(width), top = static_cast<int>(height), right = -1, bottom = -1;
        const int cx = static_cast<int>(shadow + center_x * scale);
        const int cy = static_cast<int>(shadow + 573.0f * scale);
        const int radius = static_cast<int>(14 * scale);
        for (int y = cy - radius; y <= cy + radius; ++y) for (int x = cx - radius; x <= cx + radius; ++x) {
            const auto* pixel = pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            const bool ink = dark ? std::min({pixel[0], pixel[1], pixel[2]}) > 180
                                  : std::max({pixel[0], pixel[1], pixel[2]}) < 120;
            if (pixel[3] > 180 && ink) {
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
        }
        return D2D1::SizeF(static_cast<float>(right - left + 1), static_cast<float>(bottom - top + 1));
    };
    const auto check = icon_size(82), close = icon_size(210);
    const bool balanced = check.width >= 8 * scale && close.width >= 8 * scale &&
        std::abs(check.width - close.width) <= 3.5f * scale &&
        std::abs(check.height - close.height) <= 3.5f * scale;
    if (!balanced) std::printf("[ICONS] scale=%.1f check=%.1fx%.1f close=%.1fx%.1f\n",
        scale, check.width, check.height, close.width, close.height);
    Check(balanced, "Accept and Cancel have balanced visible icon dimensions");
}

struct PickerExitSamples {
    std::atomic_bool stop = false;
    std::vector<BYTE> alpha;
    std::vector<COLORREF> corners;
    std::thread worker;
    COLORREF initial_corner = CLR_INVALID;
    bool ordinary_layered_alpha = false;
    bool started = false;
} picker_exit;

void StartPickerExitSamples(HWND picker) {
    RECT rect{}; GetWindowRect(picker, &rect);
    HDC screen = GetDC(nullptr);
    picker_exit.initial_corner = GetPixel(screen, rect.left + 3, rect.top + 3);
    ReleaseDC(nullptr, screen);
    picker_exit.started = true;
    picker_exit.worker = std::thread([picker, rect] {
        HDC desktop = GetDC(nullptr);
        while (!picker_exit.stop.load() && IsWindow(picker)) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(picker, WM_APP + 0x2BD, 0, 0, SMTO_ABORTIFHUNG, 40, &result) &&
                result > 0 && result <= 256 && IsWindowVisible(picker)) {
                picker_exit.alpha.push_back(static_cast<BYTE>(result - 1));
                BYTE alpha = 0; DWORD flags = 0;
                picker_exit.ordinary_layered_alpha |= GetLayeredWindowAttributes(picker, nullptr, &alpha, &flags) != FALSE;
                picker_exit.corners.push_back(GetPixel(desktop, rect.left + 3, rect.top + 3));
            }
            Sleep(1);
        }
        ReleaseDC(nullptr, desktop);
    });
}

void CheckPickerExitSamples() {
    picker_exit.stop.store(true);
    if (picker_exit.worker.joinable()) picker_exit.worker.join();
    BOOL motion = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
    const bool animates = motion && !pulse::ui::IsHighContrast();
    Check(picker_exit.started && (!animates || !picker_exit.alpha.empty()), "sample the real picker while closing");
    Check(!picker_exit.ordinary_layered_alpha, "exit retains per-pixel alpha instead of switching layered-window modes");
    Check(picker_exit.initial_corner == RGB(215, 231, 247), "desktop sample sees the visible fixture backdrop through the transparent corner");
    bool background_retained = picker_exit.initial_corner != CLR_INVALID;
    for (COLORREF color : picker_exit.corners) {
        background_retained &= color != CLR_INVALID &&
            std::abs(GetRValue(color) - GetRValue(picker_exit.initial_corner)) <= 8 &&
            std::abs(GetGValue(color) - GetGValue(picker_exit.initial_corner)) <= 8 &&
            std::abs(GetBValue(color) - GetBValue(picker_exit.initial_corner)) <= 8;
        std::printf("[EXIT] transparent corner=%06lx reference=%06lx\n", color, picker_exit.initial_corner);
    }
    Check(background_retained, "transparent picker corners never darken during the desktop exit frames");
    if (animates) {
        const bool intermediate = std::any_of(picker_exit.alpha.begin(), picker_exit.alpha.end(),
            [](BYTE value) { return value > 0 && value < 255; });
        Check(intermediate && std::is_sorted(picker_exit.alpha.rbegin(), picker_exit.alpha.rend()),
            "picker exit presents decreasing per-pixel opacity without a full-opacity jump");
    } else std::printf("[SKIP] intermediate exit frames: system motion disabled or high contrast enabled\n");
}

void SaveScreenBitmap(HBITMAP bitmap, const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> source;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    const bool saved = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source)) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->WriteSource(source.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit());
    Check(saved, "actual picker handoff screen capture saved");
}

bool picker_handoff_only = false;
bool picker_dark = false;
bool picker_native = false;

void CheckPickerHandoff(HWND picker) {
    SetWindowPos(picker, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    RECT bounds{};
    GetWindowRect(picker, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const float scale = (width - 40) / 292.0f;
    const auto px = [&](float value) { return 20 + static_cast<int>(std::lround(value * scale)); };
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto old = SelectObject(dc, bitmap);
    std::vector<RECT> before;
    for (int phase = 0; phase < 2; ++phase) {
        SendMessageW(picker, WM_APP + 0x2BE, phase, 0);
        RedrawWindow(picker, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        Sleep(60);
        DwmFlush();
        BitBlt(dc, 0, 0, width, height, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
        const auto field_background = GetPixel(dc, px(45), px(363));
        const int expected_background = picker_dark ? 45 : 255;
        const bool visible = std::abs(GetRValue(field_background) - expected_background) < 3 &&
            std::abs(GetGValue(field_background) - expected_background) < 3 &&
            std::abs(GetBValue(field_background) - expected_background) < 3;
        Check(visible, "handoff capture contains the visible picker field rather than another window");
        const auto name = std::wstring(picker_native ? L"handoff-native-" : L"handoff-") +
            (picker_dark ? L"dark-" : L"light-") +
            std::to_wstring(static_cast<int>(std::lround(scale * 100))) +
            (phase ? L"-live.png" : L"-painted.png");
        SaveScreenBitmap(bitmap, output / name);
        // Native fields are hidden/shown exactly as at the entrance handoff.
        // Compare actual screen glyph bounds rather than the EDIT's rectangle.
        for (int field = 0; field < 5; ++field) {
            const int left = px(field < 4 ? 48.0f : 120.0f);
            const float row_top = field < 4 ? 360.0f + field * 46 : 314.0f;
            const int top = px(row_top + 1), bottom = px(row_top + 34);
            const int right = left + static_cast<int>(64 * scale);
            RECT ink{right, bottom, left, top};
            for (int y = top; y < bottom; ++y)
                for (int x = left; x < right; ++x) {
                    const auto pixel = GetPixel(dc, x, y);
                    const bool text_pixel = picker_dark
                        ? std::min({GetRValue(pixel), GetGValue(pixel), GetBValue(pixel)}) > 180
                        : std::max({GetRValue(pixel), GetGValue(pixel), GetBValue(pixel)}) < 120;
                    if (text_pixel) {
                        ink.left = std::min(ink.left, static_cast<LONG>(x));
                        ink.top = std::min(ink.top, static_cast<LONG>(y));
                        ink.right = std::max(ink.right, static_cast<LONG>(x));
                        ink.bottom = std::max(ink.bottom, static_cast<LONG>(y));
                    }
                }
            if (!phase) before.push_back(ink);
            else {
                const auto& previous = before[field];
                const bool stable = visible && ink.right >= ink.left && previous.right >= previous.left &&
                    ink.left == previous.left && ink.top == previous.top &&
                    ink.right == previous.right && ink.bottom == previous.bottom;
                if (!stable) std::printf("[HANDOFF] field=%d before=%ld,%ld,%ld,%ld after=%ld,%ld,%ld,%ld\n",
                    field, previous.left, previous.top, previous.right, previous.bottom,
                    ink.left, ink.top, ink.right, ink.bottom);
                Check(stable, "picker text keeps identical screen glyph bounds at entrance handoff");
            }
        }
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
}

void CALLBACK DrivePicker(HWND, UINT, UINT_PTR timer, DWORD) {
    OwnedWindowSearch search{fixture_owner, L"PulseFluentColorPicker", GetCurrentProcessId(), GetCurrentThreadId()};
    EnumThreadWindows(search.thread_id, FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    const HWND picker = search.result;
    if (!MatchesOwnedWindow(picker, search)) return;
    KillTimer(nullptr, timer);
    SetFocus(picker);
    if (picker_native) {
        for (HWND edit = GetWindow(picker, GW_CHILD); edit; edit = GetWindow(edit, GW_HWNDNEXT)) {
            SetPropW(edit, L"Pulse.NativeEditFallback", reinterpret_cast<HANDLE>(1));
            SendMessageW(edit, pulse::ui::typography::TextBackendChangedMessage(), 0, 0);
        }
    }
    CheckPickerHandoff(picker);
    if (picker_handoff_only) {
        SendMessageW(picker, WM_KEYDOWN, VK_ESCAPE, 0);
        return;
    }
    SendMessageW(picker, WM_KEYDOWN, VK_TAB, 0);
    Check(GetFocus() == picker, "Tab reaches the color mode control");
    Check(Snapshot(picker, L"color-mode-focus"), "color mode focus screenshot saved");
    SendMessageW(picker, WM_KEYDOWN, VK_RETURN, 0);
    Check(IsWindow(picker) && SendMessageW(picker, WM_APP + 0x2BC, 0, 0) == 1,
        "Enter on the mode control switches to HSV without accepting the popup");
    Check(Snapshot(picker, L"color-hsv-focus"), "HSV mode screenshot saved");
    CheckPickerTextAlignment(output / L"color-hsv-focus-96.png", false, 1.0f);
    SendMessageW(picker, WM_KEYDOWN, VK_SPACE, 0);
    Check(SendMessageW(picker, WM_APP + 0x2BC, 0, 0) == 0,
        "Space on the mode control switches back to RGB");
    Tab();
    Check(GetFocus() != picker && IsChild(picker, GetFocus()), "Tab reaches the native hex edit");
    RECT edit_bounds{}; GetWindowRect(GetFocus(), &edit_bounds);
    MapWindowPoints(nullptr, picker, reinterpret_cast<POINT*>(&edit_bounds), 2);
    Check(std::abs((edit_bounds.top + edit_bounds.bottom) * 0.5f -
        (pulse::ui::FluentMenu::kShadowMargin + 332.0f)) <= 1.0f,
        "live native hex edit shares the centered row used by the painted value");
    ShiftTab();
    Check(GetFocus() == picker, "Shift+Tab returns from the hex edit to the mode control");
    Tab();
    for (int index = 0; index < 5; ++index) Tab();
    Check(GetFocus() == picker, "Tab traverses all color edits and reaches Accept");
    Check(Snapshot(picker, L"color-accept-focus"), "color Accept focus screenshot saved");
    Tab();
    Check(Snapshot(picker, L"color-cancel-focus"), "color Cancel focus screenshot saved");
    StartPickerExitSamples(picker);
    SendMessageW(picker, WM_KEYDOWN, VK_SPACE, 0);
}

void PickerStyle(HWND owner) {
    using namespace pulse;
    ui::Compositor compositor;
    Check(compositor.Init(owner), "picker compositor initializes");
    compositor.RecreateTextFormats(1);
    {
        compositor.Dc()->BeginDraw();
        compositor.Dc()->Clear(D2D1::ColorF(0.25f, 0.5f, 0.75f, 0.5f));
        Check(SUCCEEDED(compositor.Dc()->EndDraw()), "draw translucent snapshot color fixture");
        const auto path = output / L"snapshot-straight-alpha.png";
        Check(compositor.SaveSnapshot(path.c_str()), "save translucent snapshot color fixture");
        ui::ComPtr<IWICImagingFactory> factory;
        ui::ComPtr<IWICBitmapDecoder> decoder;
        ui::ComPtr<IWICBitmapFrameDecode> frame;
        ui::ComPtr<IWICFormatConverter> pixels;
        HRESULT decoded = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(decoded)) decoded = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(decoded)) decoded = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(decoded)) decoded = factory->CreateFormatConverter(&pixels);
        if (SUCCEEDED(decoded)) decoded = pixels->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
        BYTE color[4]{};
        const WICRect sample{0, 0, 1, 1};
        if (SUCCEEDED(decoded)) decoded = pixels->CopyPixels(&sample, 4, 4, color);
        Check(SUCCEEDED(decoded) && std::abs(static_cast<int>(color[0]) - 191) <= 2 &&
            std::abs(static_cast<int>(color[1]) - 128) <= 2 &&
            std::abs(static_cast<int>(color[2]) - 64) <= 2 &&
            std::abs(static_cast<int>(color[3]) - 128) <= 1,
            "PNG round trip retains straight RGB and alpha rather than darkening translucent colors");
    }
    {
        auto theme = ui::MakeHighContrastTheme();
        theme.text_secondary = ui::HexColor(0x777777);
        theme.fill_hover = ui::HexColor(0xAA0000);
        theme.text_selected = ui::HexColor(0x00FF00);
        ui::fluent::Painter painter;
        painter.SetCompositor(&compositor);
        ui::fluent::InfoBarSpec bar;
        bar.bounds = D2D1::RectF(20, 20, 620, 92);
        bar.title = L"Error notification";
        bar.message = L"Hover the Close control";
        bar.state.hovered = true;
        bar.show_close = true;
        compositor.Dc()->BeginDraw();
        compositor.Dc()->Clear(theme.bg);
        painter.BeginFrame(theme, true);
        painter.DrawInfoBar(bar);
        compositor.Dc()->EndDraw();
        Check(compositor.SaveSnapshot((output / L"notification-close-paired-hc.png").c_str()),
            "notification Close high-contrast screenshot saved");
        ui::ComPtr<ID2D1Image> target;
        ui::ComPtr<ID2D1Bitmap1> source, cpu;
        auto* dc = compositor.Dc();
        dc->GetTarget(&target);
        bool readable = target.get() && SUCCEEDED(target->QueryInterface(&source));
        if (readable) {
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                source->GetPixelFormat());
            readable = SUCCEEDED(dc->CreateBitmap(source->GetPixelSize(), nullptr, 0, properties, &cpu)) &&
                SUCCEEDED(cpu->CopyFromBitmap(nullptr, source.get(), nullptr));
        }
        D2D1_MAPPED_RECT mapped{};
        readable = readable && SUCCEEDED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        int paired_pixels = 0;
        if (readable) {
            const int right = static_cast<int>(bar.bounds.right - bar.close_inset);
            for (int y = 20; y < 92; ++y) for (int x = right - 28; x < right; ++x) {
                const BYTE* pixel = mapped.bits + y * mapped.pitch + x * 4;
                if (pixel[1] > 160 && pixel[2] < 80 && pixel[0] < 80) ++paired_pixels;
            }
            cpu->Unmap();
        }
        Check(readable && paired_pixels > 3,
            "hovered high-contrast Close glyph uses the selected foreground at actual pixels");
    }
    for (const bool dark : {false, true}) {
        for (const float scale : {1.0f, 1.5f}) {
            const auto file = output / (std::wstring(L"color-picker-") + (dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 96)) + L".png");
            Check(ui::ColorPickerPopup::SaveDebugSnapshot(&compositor, file.c_str(), dark,
                ui::HexColor(0x0078D4), scale), "picker theme/DPI screenshot saved");
            CheckPickerTextAlignment(file, dark, scale);
        }
    }
    const auto contrast_file = output / L"color-picker-high-contrast.png";
    Check(ui::ColorPickerPopup::SaveDebugSnapshot(&compositor, contrast_file.c_str(), false,
        ui::HexColor(0x0078D4), 1, true), "picker forced high-contrast hover screenshot saved");
    drive_timer = SetTimer(nullptr, 0, 350, DrivePicker);
    uint32_t color = 0x3574BA;
    const bool accepted = ui::ColorPickerPopup::Pick(owner, &compositor, nullptr, 1, {100, 80},
        color, false, color);
    CheckPickerExitSamples();
    KillTimer(nullptr, drive_timer);
    Check(!accepted && color == 0x3574BA, "keyboard Cancel preserves the original color");
    picker_handoff_only = true;
    for (const auto& sample : {std::pair{true, 1.0f}, std::pair{false, 1.25f}, std::pair{true, 1.5f}}) {
        picker_dark = sample.first;
        drive_timer = SetTimer(nullptr, 0, 350, DrivePicker);
        const bool result = ui::ColorPickerPopup::Pick(owner, &compositor, nullptr, sample.second,
            {100, 80}, color, picker_dark, color);
        KillTimer(nullptr, drive_timer);
        Check(!result && color == 0x3574BA, "theme/DPI handoff check preserves the canceled color");
    }
    picker_native = true;
    picker_dark = true;
    drive_timer = SetTimer(nullptr, 0, 350, DrivePicker);
    const bool native_result = ui::ColorPickerPopup::Pick(owner, &compositor, nullptr, 1,
        {100, 80}, color, picker_dark, color);
    KillTimer(nullptr, drive_timer);
    Check(!native_result && color == 0x3574BA, "native editor fallback handoff preserves the canceled color");
    compositor.Shutdown();
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    bool preview_style_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--preview-style-only") preview_style_only = true;
        if (std::wstring_view(argv[i]) == L"--dialog-buttons-only") dialog_buttons_only = true;
        if (std::wstring_view(argv[i]) == L"--dialog-titles-only") dialog_titles_only = dialog_buttons_only = true;
        if (std::wstring_view(argv[i]) == L"--color-picker-only") color_picker_only = true;
    }
    compat::EnableDpiAwareness(); OleInitialize(nullptr); InitCommonControls();
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (preview_style_only) {
        HWND owner = CreateWindowExW(0, L"STATIC", L"Preview style fixture", WS_OVERLAPPEDWINDOW,
            20, 20, 1000, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(owner, SW_SHOW);
        ui::QuickPreviewPlaybackProbe::Style(owner);
        DestroyWindow(owner); OleUninitialize();
        std::printf("failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    output = std::filesystem::absolute(color_picker_only ? L"bench_data/popup-style/color-picker" : dialog_buttons_only ? L"bench_data/ui-consistency-fix/dialogs"
        : L"bench_data/audit-fixes-20261030-remaining/ui-build/screenshots");
    std::filesystem::create_directories(output);
    const auto left = output / L"left.txt", right = output / L"right.txt";
    Write(left, "alpha\nline two\n"); Write(right, "Alpha\nline three\n");
    WNDCLASSW backdrop{};
    HBRUSH backdrop_brush = nullptr;
    if (color_picker_only) {
        backdrop.lpfnWndProc = DefWindowProcW;
        backdrop.hInstance = GetModuleHandleW(nullptr);
        backdrop.lpszClassName = L"PulsePickerExitBackdropFixture";
        backdrop_brush = CreateSolidBrush(RGB(215, 231, 247));
        backdrop.hbrBackground = backdrop_brush;
        RegisterClassW(&backdrop);
    }
    HWND owner = CreateWindowExW(0, color_picker_only ? backdrop.lpszClassName : L"STATIC", L"Pulse isolated UI fixture", WS_OVERLAPPEDWINDOW,
        20, 20, 1000, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    fixture_owner = owner;
    ShowWindow(owner, SW_SHOW);
    if (color_picker_only) {
        SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        PickerStyle(owner);
        DestroyWindow(owner);
        UnregisterClassW(backdrop.lpszClassName, backdrop.hInstance);
        DeleteObject(backdrop_brush);
        OleUninitialize();
        std::printf("failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    for (bool dark : {false, true}) {
        if (dark && !dialog_buttons_only) break;
        dialog_dark = dark;
        for (int resolution : {96, 144, 168, 192}) {
            if (!dialog_titles_only && resolution > 144) continue;
            dpi = resolution;
            if (!dialog_buttons_only || dialog_titles_only) {
                batch = false; active_class = L"PulseAdvancedSearchWindow";
                drive_timer = SetTimer(nullptr, 0, 220, Drive);
                model::AdvancedSearchSpec spec; spec.current_folder = output.wstring();
                spec.location = model::LocationScope::CurrentFolder; spec.content = L"alpha";
                const auto search = ui::ShowAdvancedSearchDialog(owner, spec, dark, ui::HexColor(0x3574BA));
                if (dialog_titles_only) Check(!search.accepted, "title fixture closes advanced search without submitting a query");
                else Check(search.accepted && search.query.find(L"ww:") != std::wstring::npos &&
                    search.query.find(L"case:") != std::wstring::npos, "keyboard checkboxes affect the compiled advanced query");
            }
            batch = true; active_class = L"PulseBatchRenameWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
            const auto renamed = ui::ShowBatchRenameDialog(owner, {left.wstring()}, dark,
                ui::HexColor(dialog_buttons_only ? 0x0078D4 : 0x3574BA));
            Check(!renamed.accepted, "batch fixture closes without performing a rename");
            if (dialog_buttons_only && !dialog_titles_only) {
                submit_batch = true;
                drive_timer = SetTimer(nullptr, 0, 220, Drive);
                const auto plan = ui::ShowBatchRenameDialog(owner, {left.wstring()}, dark, ui::HexColor(0x0078D4));
                Check(plan.accepted && plan.items.size() == 1 && plan.items[0].new_name == L"renamed-left.txt",
                    "batch primary submits the previewed rename plan without changing fixture files");
                Check(std::filesystem::exists(left) && !std::filesystem::exists(output / L"renamed-left.txt"),
                    "batch dialog confirmation leaves fixture files unchanged");
                submit_batch = false;
            }
            batch = false; active_class = L"PulseConnectServerWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
            ui::ConnectServerDialogSpec server; server.title = L"Connect fixture"; server.label = L"Address";
            server.initial_text = L"\\\\fixture\\share"; server.confirm_text = L"Connect";
            server.preview = [](const auto& text) {
                return ui::ConnectServerPreview{!dialog_buttons_only || !text.empty(), L"Controlled preview"};
            };
            const auto connected = ui::ShowConnectServerDialog(owner, server, dark,
                ui::HexColor(dialog_buttons_only ? 0x0078D4 : 0x3574BA));
            if (dialog_titles_only) Check(!connected.accepted, "title fixture closes server dialog without connecting");
            else Check(connected.accepted && !connected.pin, "keyboard server pin activation changes the submitted result");
            if (dialog_buttons_only) continue;
            active_class = L"PulseFileConflictWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
            ops::ConflictItemInfo conflict; conflict.source = left.wstring(); conflict.destination = right.wstring();
            conflict.remaining = 2;
            const auto decision = ui::ShowFileConflictDialog(owner, conflict, false, ui::HexColor(0x3574BA));
            Check(decision.choice == ops::ConflictChoice::Cancel, "conflict fixture cancels without changing files");
        }
    }
    if (dialog_buttons_only) {
        DestroyWindow(owner); OleUninitialize();
        std::printf("failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    dpi = 96;
    ui::QuickPreviewPlaybackProbe::Run(owner);
    ui::FileOperationFailureProbe::Run(owner);
    ui::TextDiffWindowTestPeer::Run(owner, left, right);
    ui::Compositor compositor; Check(compositor.Init(owner), "color/menu compositor initializes");
    compositor.RecreateTextFormats(1);
    ui::FluentMenuTestPeer::Run(owner, compositor);
    ui::FluentMenuTestPeer::HighContrast(compositor);
    std::atomic<ULONGLONG> delivered{0};
    const DWORD ui_thread_id = GetCurrentThreadId();
    const DWORD fixture_process_id = GetCurrentProcessId();
    std::thread escape([&, ui_thread_id, fixture_process_id, picker_owner = owner] {
        const auto deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) {
            OwnedWindowSearch search{picker_owner, L"PulseFluentColorPicker", fixture_process_id, ui_thread_id};
            EnumThreadWindows(ui_thread_id, FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
            const HWND picker = search.result;
            if (MatchesOwnedWindow(picker, search)) {
                const auto start = GetTickCount64();
                SendMessageW(picker, WM_KEYDOWN, VK_ESCAPE, 0);
                delivered = GetTickCount64() - start + 1; return;
            }
            Sleep(1);
        }
    });
    uint32_t color = 0;
    const bool picked = ui::ColorPickerPopup::Pick(owner, &compositor, nullptr, 1, {100, 80}, 0x3574BA, false, color);
    escape.join();
    std::printf("color Escape dispatch latency=%llu ms\n", static_cast<unsigned long long>(delivered.load() - 1));
    Check(!picked && color == 0x3574BA && delivered > 0 && delivered < 100,
        "color picker dispatches Escape during its entrance within 100 ms");
    compositor.Shutdown(); DestroyWindow(owner); OleUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
