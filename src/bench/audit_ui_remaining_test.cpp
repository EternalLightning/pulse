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
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <commctrl.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <wrl/client.h>

namespace {
int failures = 0;
int dpi = 96;
UINT_PTR drive_timer = 0;
std::wstring active_class;
HWND fixture_owner = nullptr;
bool batch = false;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}
std::filesystem::path output;
bool Snapshot(HWND window, const std::wstring& name) {
    const auto path = output / (name + L"-" + std::to_wstring(dpi) + L".png");
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
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    OwnedWindowSearch search{fixture_owner, active_class.c_str(), GetCurrentProcessId(), GetCurrentThreadId()};
    EnumThreadWindows(search.thread_id, FindOwnedWindow, reinterpret_cast<LPARAM>(&search));
    const HWND dialog = search.result;
    if (!MatchesOwnedWindow(dialog, search)) return;
    KillTimer(nullptr, drive_timer);
    RECT bounds{}; GetWindowRect(dialog, &bounds);
    const LONG width = active_class == L"PulseConnectServerWindow" ? 520 : batch ? 620 : 560;
    const bool conflict = active_class == L"PulseFileConflictWindow";
    const LONG height = active_class == L"PulseConnectServerWindow" ? 348 : batch ? 588 : conflict ? 428 : 500;
    bounds.right = bounds.left + (conflict ? 480 : width) * dpi / 96;
    bounds.bottom = bounds.top + height * dpi / 96;
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
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

int wmain() {
    using namespace pulse;
    compat::EnableDpiAwareness(); OleInitialize(nullptr); InitCommonControls();
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    output = std::filesystem::absolute(L"bench_data/audit-fixes-20261030-remaining/ui-build/screenshots");
    std::filesystem::create_directories(output);
    const auto left = output / L"left.txt", right = output / L"right.txt";
    Write(left, "alpha\nline two\n"); Write(right, "Alpha\nline three\n");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse isolated UI fixture", WS_OVERLAPPEDWINDOW,
        20, 20, 1000, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    fixture_owner = owner;
    ShowWindow(owner, SW_SHOW);
    for (int resolution : {96, 144}) {
        dpi = resolution; batch = false; active_class = L"PulseAdvancedSearchWindow";
        drive_timer = SetTimer(nullptr, 0, 220, Drive);
        model::AdvancedSearchSpec spec; spec.current_folder = output.wstring();
        spec.location = model::LocationScope::CurrentFolder; spec.content = L"alpha";
        const auto search = ui::ShowAdvancedSearchDialog(owner, spec, false, ui::HexColor(0x3574BA));
        Check(search.accepted && search.query.find(L"ww:") != std::wstring::npos &&
            search.query.find(L"case:") != std::wstring::npos, "keyboard checkboxes affect the compiled advanced query");
        batch = true; active_class = L"PulseBatchRenameWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
        const auto renamed = ui::ShowBatchRenameDialog(owner, {left.wstring()}, false, ui::HexColor(0x3574BA));
        Check(!renamed.accepted, "batch fixture closes without performing a rename");
        batch = false; active_class = L"PulseConnectServerWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
        ui::ConnectServerDialogSpec server; server.title = L"Connect fixture"; server.label = L"Address";
        server.initial_text = L"\\\\fixture\\share"; server.confirm_text = L"Connect";
        server.preview = [](const auto&) { return ui::ConnectServerPreview{true, L"Controlled preview"}; };
        const auto connected = ui::ShowConnectServerDialog(owner, server, false, ui::HexColor(0x3574BA));
        Check(connected.accepted && !connected.pin, "keyboard server pin activation changes the submitted result");
        active_class = L"PulseFileConflictWindow"; drive_timer = SetTimer(nullptr, 0, 220, Drive);
        ops::ConflictItemInfo conflict; conflict.source = left.wstring(); conflict.destination = right.wstring();
        conflict.remaining = 2;
        const auto decision = ui::ShowFileConflictDialog(owner, conflict, false, ui::HexColor(0x3574BA));
        Check(decision.choice == ops::ConflictChoice::Cancel, "conflict fixture cancels without changing files");
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
