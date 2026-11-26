// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/archive_dialog.h"
#include "../app/archive_operation_ui.h"
#include "../common/localization.h"
#include "../ui/FluentTokens.h"
#include "../ui/typography.h"
#include "../ui/input_dialog.h"
#include "../ui/edit_host.h"
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "../common/windows_compat.h"
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <mutex>

namespace {
constexpr int kDestination = 100, kPassword = 101, kCreateFolder = 103;
constexpr COLORREF kAccent = RGB(80, 180, 220);
COLORREF dialog_accent = kAccent;
std::atomic_int failures = 0;

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
std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::uint64_t EditPixels(HWND dialog, HWND edit, bool dark) {
    const auto info = Inspect(dialog);
    bool custom = false;
    for (const auto& field : info.fields) if (field.edit == edit) custom = field.custom;
    RECT bounds{}; GetClientRect(edit, &bounds);
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = bounds.right;
    bitmap_info.bmiHeader.biHeight = -bounds.bottom;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    HDC source = GetDC(edit), memory = CreateCompatibleDC(source);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    std::uint64_t hash = 0;
    if (bitmap && memory && pixels) {
        const auto previous = SelectObject(memory, bitmap);
        const bool rendered = custom && info.compositor
            ? info.compositor->PaintLumaEdit(edit, memory, info.compositor->TextFormat(),
                pulse::ui::ChildEditColor(pulse::ui::ChildEditTextColor(dark)),
                pulse::ui::ChildEditColor(pulse::ui::ChildEditBackColor(dark)))
            : PrintWindow(edit, memory, PW_CLIENTONLY) != FALSE;
        if (rendered) {
            GdiFlush();
            hash = 14695981039346656037ull;
            const auto* bytes = static_cast<const BYTE*>(pixels);
            for (size_t index = 0; index < static_cast<size_t>(bounds.right) * bounds.bottom * 4; ++index)
                if (index % 4 != 3) hash = (hash ^ bytes[index]) * 1099511628211ull;
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (source) ReleaseDC(edit, source);
    return hash;
}

void CheckPasswordMask(HWND dialog, bool dark) {
    OnUi(dialog, [&] {
        const HWND edit = GetDlgItem(dialog, kPassword);
        const auto retained = Text(edit);
        SetFocus(edit);
        HideCaret(edit);
        SetWindowTextW(edit, L"");
        const auto blank = EditPixels(dialog, edit, dark);
        for (const auto selection : {std::pair{0, 0}, std::pair{3, 3}, std::pair{6, 6},
            std::pair{1, 5}, std::pair{0, 6}}) {
            SetWindowTextW(edit, L"iii123");
            SendMessageW(edit, EM_SETSEL, selection.first, selection.second);
            const auto first = EditPixels(dialog, edit, dark);
            Check(Text(edit) == L"iii123", "masked password retains its original WM_GETTEXT value");
            SetWindowTextW(edit, L"WWWxyz");
            SendMessageW(edit, EM_SETSEL, selection.first, selection.second);
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            const auto second = EditPixels(dialog, edit, dark);
            Check(blank != 0 && first != blank && first == second && Text(edit) == L"WWWxyz",
                "same-length distinct passwords render identical visible masks at caret and selection boundaries");
            Check(start == static_cast<DWORD>(selection.first) && end == static_cast<DWORD>(selection.second),
                "password masking preserves native caret and selection positions");
        }
        SetWindowTextW(edit, retained.c_str());
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(-1), -1);
        SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(IsWindow(dialog) && Text(edit) == retained,
            "synthetic IME composition Enter and Escape preserve the open password editor");
        SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
        const auto info = Inspect(dialog);
        std::vector<int> actions;
        if (info.browse.right > info.browse.left) actions.push_back(102);
        if (info.checkbox.right > info.checkbox.left) actions.push_back(kCreateFolder);
        actions.push_back(IDCANCEL); actions.push_back(IDOK); actions.push_back(104);
        for (const int action : actions) {
            SendMessageW(GetFocus(), WM_KEYDOWN, VK_TAB, 0);
            Check(GetFocus() == dialog && Inspect(dialog).focus == action,
                "archive Tab traverses browse, checkbox and shared footer actions in visual order");
        }
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        Check(GetFocus() == info.fields.front().edit, "archive Tab wraps from Close to the first native editor");
        SetFocus(edit);
        ShowCaret(edit);
    });
}

struct Search {
    HWND owner = nullptr;
    HWND dialog = nullptr;
};

BOOL CALLBACK FindDialog(HWND hwnd, LPARAM parameter) {
    auto& search = *reinterpret_cast<Search*>(parameter);
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if ((std::wstring(name) == L"PulseArchiveDialog" || std::wstring(name) == L"PulseFileOperationWindow") &&
        GetWindow(hwnd, GW_OWNER) == search.owner && IsWindowVisible(hwnd)) {
        search.dialog = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Enumerate only this test's UI thread and require this test's owner window.
std::thread Automate(DWORD ui_thread, HWND owner, std::function<void(HWND)> action, std::function<void()> timed_out = {}) {
    return std::thread([ui_thread, owner, action = std::move(action), timed_out = std::move(timed_out)] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        Search search{owner};
        while (std::chrono::steady_clock::now() < deadline) {
            EnumThreadWindows(ui_thread, FindDialog, reinterpret_cast<LPARAM>(&search));
            if (search.dialog) {
                action(search.dialog);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(false, "dialog appeared on the test UI thread within timeout");
        if (timed_out) timed_out();
        PostThreadMessageW(ui_thread, WM_QUIT, 1, 0);
    });
}

void Confirm(HWND dialog) { SendMessageW(dialog, WM_COMMAND, IDOK, 0); }
void Cancel(HWND dialog) { SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); }

bool SaveScreenshot(HWND dialog, const wchar_t* name) {
    pulse::ui::InputDialogInspection inspection;
    if (SendMessageW(dialog, pulse::ui::kInputDialogInspectMessage, 0, reinterpret_cast<LPARAM>(&inspection))) {
        const bool dark = std::wstring(name).find(L"dark") != std::wstring::npos;
        const auto theme = inspection.high_contrast ? pulse::ui::MakeHighContrastTheme() :
            pulse::ui::MakeTheme(dark, D2D1::ColorF(GetRValue(dialog_accent) / 255.0f,
                GetGValue(dialog_accent) / 255.0f, GetBValue(dialog_accent) / 255.0f));
        const auto input = pulse::ui::BlendOver(theme.fill_input_focus, pulse::ui::BlendOver(theme.surface_sheet, theme.bg));
        const auto outline = inspection.high_contrast ? theme.stroke_card : dark
            ? pulse::ui::HexColor(0xFFFFFF, 0.08f) : pulse::ui::HexColor(0x000000, 13.0f / 255.0f);
        GUITHREADINFO thread{sizeof(thread)};
        GetGUIThreadInfo(GetWindowThreadProcessId(dialog, nullptr), &thread);
        for (const auto& field : inspection.fields) {
            const auto border = pulse::ui::BlendOver(outline, input);
            Check(Pixel(dialog, field.bounds.left, field.bounds.top + 8 * inspection.scale,
                border), "archive input outline composites over the rendered fill");
            Check(Pixel(dialog, field.bounds.left + 30 * inspection.scale, field.bounds.bottom - 1,
                thread.hwndFocus == field.edit ? pulse::ui::BlendOver(theme.accent, input) :
                    pulse::ui::BlendOver(theme.stroke_input_bottom, border)),
                "archive accent underline belongs only to the focused editor");
        }
        std::error_code error;
        const auto folder = std::filesystem::path(L"bench_data/archive_ui");
        std::filesystem::create_directories(folder, error);
        const auto output = std::filesystem::absolute(folder / (std::wstring(name) + L"-" +
            std::to_wstring(pulse::compat::WindowDpi(dialog)) + L"dpi.png"));
        return !error && SendMessageW(dialog, pulse::ui::kInputDialogSnapshotMessage, 0,
            reinterpret_cast<LPARAM>(output.c_str())) != 0;
    }
    RECT bounds{};
    GetWindowRect(dialog, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(dialog), memory = CreateCompatibleDC(screen);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool saved = false;
    if (bitmap && pixels && memory) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        // These custom windows paint WM_PAINT, so capture their displayed client pixels.
        if (BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY)) {
            std::error_code error;
            const std::filesystem::path folder = L"bench_data/archive_ui";
            std::filesystem::create_directories(folder, error);
            const std::filesystem::path output = folder /
                (std::wstring(name) + L"-" + std::to_wstring(pulse::compat::WindowDpi(dialog)) + L"dpi.bmp");
            if (!error) {
                HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    const DWORD pixel_bytes = static_cast<DWORD>(width * height * 4);
                    BITMAPFILEHEADER header{};
                    header.bfType = 0x4D42;
                    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
                    header.bfSize = header.bfOffBits + pixel_bytes;
                    DWORD written = 0;
                    saved = WriteFile(file, &header, sizeof(header), &written, nullptr) &&
                        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
                        WriteFile(file, pixels, pixel_bytes, &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(dialog, screen);
    return saved;
}

void CheckArchiveStyle(HWND dialog, UINT dpi) {
    const auto px = [dpi](int dip) { return MulDiv(dip, static_cast<int>(dpi), 96); };
    RECT client{}; GetClientRect(dialog, &client);
    const auto info = Inspect(dialog);
    const auto primary = info.primary, cancel = info.cancel, close = info.close;
    Check(close.right - close.left == px(46) && close.bottom == px(44),
        "archive close target follows 46 by 44 DIP header contract");
    Check(primary.bottom - primary.top == px(32) && cancel.bottom == primary.bottom &&
        primary.left - cancel.right == px(8) && client.right - primary.right == px(20) &&
        client.bottom - primary.bottom == px(16), "archive actions use right-aligned Cancel then Primary with 64 DIP footer");
    POINT point{px(50), px(10)}; ClientToScreen(dialog, &point);
    Check(SendMessageW(dialog, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCAPTION,
        "archive semantic header remains draggable");
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(GetDlgItem(dialog, 101), WM_GETFONT, 0, 0)), sizeof(font), &font);
    Check(font.lfHeight == -px(14), "archive native input uses 14 DIP text after layout");
    Check(wcscmp(font.lfFaceName, pulse::ui::typography::PreferredTextFamily()) == 0,
        "archive native input uses the shared text family");
    Check(info.title_font_size == px(14) && info.title_weight == DWRITE_FONT_WEIGHT_BOLD,
        "archive caption uses the shared bold DirectWrite title format");
    Check(CloseGlyph(dialog), "archive vector Close is visible at its shared size and centered in its hit target");
}

void CheckArchiveDpi(HWND dialog, UINT dpi, bool password_only, const wchar_t* screenshot) {
    const std::wstring password = Text(GetDlgItem(dialog, kPassword));
    const HWND destination = GetDlgItem(dialog, kDestination);
    const std::wstring path = destination ? Text(destination) : L"";
    const auto info = Inspect(dialog);
    const int height = password_only ? 224 : info.checkbox.right > info.checkbox.left ? 348 : 306;
    RECT rect{}; GetWindowRect(dialog, &rect);
    rect.right = rect.left + MulDiv(password_only ? 420 : 520, static_cast<int>(dpi), 96);
    rect.bottom = rect.top + MulDiv(height, static_cast<int>(dpi), 96);
    SendMessageW(dialog, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    Check(Text(GetDlgItem(dialog, kPassword)) == password && (!destination || Text(destination) == path),
        "archive DPI transition preserves password and destination controls");
    CheckArchiveStyle(dialog, dpi);
    Check(SaveScreenshot(dialog, screenshot), "save archive DPI-transition style screenshot");
}

std::atomic_bool theme_refresh_ran = false;
bool theme_refresh_required = false;
constexpr UINT_PTR kThemeRefreshTimer = 947;
void CALLBACK RefreshThemeFixture(HWND owner, UINT, UINT_PTR, DWORD) {
    Search search{owner};
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&search));
    if (!search.dialog) return;
    KillTimer(owner, kThemeRefreshTimer);
    const HWND dialog = search.dialog;
    const HWND edit = GetDlgItem(dialog, kPassword);
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
        Check(Inspect(dialog).high_contrast && !(GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CAPTION) && CloseGlyph(dialog),
            "high contrast keeps the shared caption and visible vector Close");
        CheckPasswordMask(dialog, false);
    }
    {
        pulse::ui::HighContrastFrameScope contrast(system_contrast);
        SendMessageW(dialog, WM_THEMECHANGED, 0, 0);
    }
    wchar_t retained[128]{}; GetWindowTextW(edit, retained, ARRAYSIZE(retained));
    Check(wcscmp(text, retained) == 0, "theme and contrast refresh preserve entered native edit text");
    theme_refresh_ran = true;
}

void RunExtractCases(HWND owner, DWORD thread) {
    for (bool dark : {false, true}) {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [dark](HWND dialog) {
            if (theme_refresh_required) {
                const auto theme_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!theme_refresh_ran.load() && std::chrono::steady_clock::now() < theme_deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(Text(GetDlgItem(dialog, kDestination)) == L"C:\\archive-dialog-fixture",
                "all: destination defaults to archive parent");
            Check(!Inspect(dialog).checkbox_text.empty(), "all: create-folder control exists");
            Check((GetWindowLongPtrW(GetDlgItem(dialog, kPassword), GWL_STYLE) & ES_PASSWORD) != 0,
                "all: password edit masks text");
            Check(Inspect(dialog).checkbox_text.find(L"example") != std::wstring::npos,
                "all: folder label uses archive stem");
            CheckArchiveStyle(dialog, pulse::compat::WindowDpi(dialog));
            CheckPasswordMask(dialog, dark);
            Check(SaveScreenshot(dialog, dark ? L"extract-dark" : L"extract-light"),
                "extraction screenshot saved at actual monitor DPI");
            CheckArchiveDpi(dialog, dark ? 192u : 144u, false, dark ? L"extract-dark-style192" : L"extract-light-style144");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, dark, dialog_accent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(accepted && options.create_folder && options.destination == L"C:\\archive-dialog-fixture",
            "all: accepting preserves default create-folder option");
    }
    {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [](HWND dialog) {
            const auto checkbox = Inspect(dialog).checkbox;
            const LPARAM point = MAKELPARAM(static_cast<int>((checkbox.left + checkbox.right) / 2),
                static_cast<int>((checkbox.top + checkbox.bottom) / 2));
            SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
            SendMessageW(dialog, WM_LBUTTONUP, 0, point);
            Check(!Inspect(dialog).checked, "create-folder checkbox toggles through actual mouse messages");
            SetWindowTextW(GetDlgItem(dialog, kPassword), L"fixture-password");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, true, dialog_accent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(accepted && !options.create_folder && options.password == L"fixture-password",
            "all: toggling checkbox and entering password updates options");
    }
    {
        pulse::app::ArchiveExtractOptions options;
        auto helper = Automate(thread, owner, [](HWND dialog) {
            Check(Inspect(dialog).checkbox_text.empty(), "selected: checkbox is absent");
            Confirm(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, false, dialog_accent,
            L"C:\\archive-dialog-fixture\\example.zip", false, options);
        helper.join();
        Check(accepted && !options.create_folder, "selected: extracts directly to destination");
    }
    {
        pulse::app::ArchiveExtractOptions options{L"C:\\original", L"original-password", false};
        auto helper = Automate(thread, owner, [](HWND dialog) {
            SetWindowTextW(GetDlgItem(dialog, kDestination), L"C:\\changed");
            SetWindowTextW(GetDlgItem(dialog, kPassword), L"changed-password");
            SendMessageW(dialog, WM_COMMAND, kCreateFolder, 0);
            Cancel(dialog);
        });
        const bool accepted = pulse::app::ShowArchiveExtractDialog(owner, false, dialog_accent,
            L"C:\\archive-dialog-fixture\\example.zip", true, options);
        helper.join();
        Check(!accepted && options.destination == L"C:\\original" &&
            options.password == L"original-password" && !options.create_folder,
            "cancel: options remain unchanged");
    }
}

void RunPasswordCases(HWND owner, DWORD thread) {
    std::wstring password = L"initial";
    auto helper = Automate(thread, owner, [](HWND dialog) {
        Check((GetWindowLongPtrW(GetDlgItem(dialog, kPassword), GWL_STYLE) & ES_PASSWORD) != 0,
            "password dialog: input masks text");
        Check(Text(GetDlgItem(dialog, kPassword)) == L"initial", "password dialog: initial value retained");
        SetWindowTextW(GetDlgItem(dialog, kPassword), L"entered");
        CheckArchiveStyle(dialog, pulse::compat::WindowDpi(dialog));
        CheckPasswordMask(dialog, true);
        Check(SaveScreenshot(dialog, L"password-dark"), "capture password prompt with semantic lock header");
        CheckArchiveDpi(dialog, 144u, true, L"password-dark-style144");
        Confirm(dialog);
    });
    const bool accepted = pulse::app::ShowArchivePasswordDialog(owner, true, dialog_accent, password);
    helper.join();
    Check(accepted && password == L"entered", "password dialog: confirms entered value");
    helper = Automate(thread, owner, [](HWND dialog) {
        SetWindowTextW(GetDlgItem(dialog, kPassword), L"discarded");
        CheckArchiveDpi(dialog, 192u, true, L"password-light-style192");
        CheckPasswordMask(dialog, false);
        Cancel(dialog);
    });
    const bool cancelled = pulse::app::ShowArchivePasswordDialog(owner, false, dialog_accent, password);
    helper.join();
    Check(!cancelled && password == L"entered", "password dialog: cancellation retains output");
}

constexpr UINT kActivateTestWindow = WM_APP + 0x390;
LRESULT CALLBACK FocusTestProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR data) {
    if (message == kActivateTestWindow) {
        const auto target = reinterpret_cast<HWND>(wp);
        SetForegroundWindow(target);
        SetActiveWindow(target);
        SetFocus(target);
        return GetActiveWindow() == target && GetFocus() == target;
    }
    if (message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && data)
        ++*reinterpret_cast<int*>(data);
    return DefSubclassProc(window, message, wp, lp);
}

void RunFocusCases(HWND owner, DWORD thread) {
    using namespace pulse;
    int other_activations = 0;
    HWND other = CreateWindowExW(0, L"STATIC", L"Unrelated focus test window",
        WS_OVERLAPPEDWINDOW, 140, 140, 700, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(other != nullptr, "create unrelated activation target");
    if (!other) return;
    SetWindowSubclass(owner, FocusTestProc, 1, 0);
    SetWindowSubclass(other, FocusTestProc, 1, reinterpret_cast<DWORD_PTR>(&other_activations));
    ShowWindow(other, SW_SHOWNOACTIVATE);
    ShowWindow(owner, SW_SHOW);
    for (int mode = 0; mode != 6; ++mode) {
        Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(owner), 0) != 0,
            "owner active and focused before modal extraction");
        other_activations = 0;
        std::atomic_bool done = false, cancel = false;
        HWND foreground_before_close = nullptr;
        bool had_foreground = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(dialog), 0) != 0,
                "progress window active and focused before close");
            if (mode == 4)
                Check(SendMessageW(owner, kActivateTestWindow, reinterpret_cast<WPARAM>(other), 0) != 0,
                    "user switches to unrelated window before worker completes");
            foreground_before_close = GetForegroundWindow();
            had_foreground = foreground_before_close == (mode == 4 ? other : dialog);
            if (mode < 3 || mode == 5) {
                if (mode == 0) {
                    RECT bounds{};
                    GetClientRect(dialog, &bounds);
                    const auto dpi = compat::WindowDpi(dialog);
                    const LPARAM point = MAKELPARAM(bounds.right - MulDiv(35, dpi, 96),
                        bounds.bottom - MulDiv(26, dpi, 96));
                    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    SendMessageW(dialog, WM_LBUTTONUP, 0, point);
                } else {
                    SendMessageW(dialog, mode == 1 ? WM_KEYDOWN : WM_CLOSE,
                        mode == 1 ? VK_ESCAPE : 0, 0);
                }
                if (mode < 3) {
                    Check(cancel.load() == (mode == 0), "only the Cancel button requests cancellation");
                    Check(IsWindow(dialog) && (mode == 0 ? !IsWindowEnabled(owner)
                        : IsWindowEnabled(owner) && !IsWindowVisible(dialog)),
                        "cancel retains the modal owner while dismiss releases it for background work");
                }
            }
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel, [&] {
            ops::OpStatus status;
            status.active = mode != 5 && !done.load();
            status.phase = mode == 5 ? ops::OpPhase::Failed : ops::OpPhase::Running;
            status.summary = L"Focus regression";
            return status;
        });
        helper.join();
        std::printf("Focus mode %d: owner=%p other=%p active=%p focus=%p foreground=%p\n",
            mode, static_cast<void*>(owner), static_cast<void*>(other),
            static_cast<void*>(GetActiveWindow()), static_cast<void*>(GetFocus()),
            static_cast<void*>(GetForegroundWindow()));
        Check(IsWindowEnabled(owner), "owner re-enabled after extraction window closes");
        Check(completed == (mode != 0), "focus restoration preserves cancellation result");
        if (mode == 4) {
            Check(GetActiveWindow() == other && GetFocus() == other,
                "background completion preserves user's active unrelated window");
        } else {
            Check(GetActiveWindow() == owner && GetFocus() == owner,
                "closing active modal progress returns activation and keyboard focus to owner");
            Check(other_activations == 0, "unrelated underlying window never activates during modal teardown");
        }
        if (had_foreground) {
            Check(GetForegroundWindow() == (mode == 4 ? other : owner),
                "foreground extraction teardown preserves the expected foreground window");
        } else {
            std::printf("[LIMIT] Windows did not grant the test foreground activation; checking external foreground preservation.\n");
            Check(GetForegroundWindow() == foreground_before_close,
                "background test does not steal foreground from an external application");
        }
    }
    RemoveWindowSubclass(owner, FocusTestProc, 1);
    DestroyWindow(other);
}

void RunProgressCases(HWND owner, DWORD thread, bool = false) {
    using namespace pulse;
    ops::ArchiveProgress progress;
    auto mapped = app::ArchiveOperationStatus(progress, L"C:\\fixtures\\example.zip", L"C:\\output", false);
    Check(mapped.phase == ops::OpPhase::Scanning && mapped.active && mapped.percent < 0 &&
        mapped.source_label == L"C:\\fixtures\\example.zip" && mapped.destination_label == L"C:\\output",
        "progress adapter preserves endpoints and unknown preparation progress");
    progress.phase = ops::ArchivePhase::Extracting;
    progress.percent = 42; progress.total_bytes = 1000; progress.processed_bytes = 420;
    progress.total_items = 10; progress.bytes_per_second = 128; progress.eta_seconds = 5;
    mapped = app::ArchiveOperationStatus(progress, L"archive.zip", L"output", false);
    Check(mapped.phase == ops::OpPhase::Running && mapped.active && mapped.percent == 42 &&
        mapped.total_bytes == 1000 && mapped.transferred_bytes == 420 && mapped.total_items == 10 &&
        mapped.completed_items == 0 && mapped.current_item.empty() && mapped.bytes_per_second == 128 && mapped.eta_seconds == 5,
        "progress adapter carries byte estimates without inventing completed items or current filenames");
    mapped = app::ArchiveOperationStatus(progress, L"archive.zip", L"output", true);
    Check(mapped.phase == ops::OpPhase::Cancelling && mapped.bytes_per_second == 0 && mapped.eta_seconds == 0,
        "cancelling adapter suppresses speed and ETA");
    for (bool dark : {false, true}) {
        std::atomic_bool done = false, cancel = false;
        ops::ArchiveProgress status;
        std::mutex status_mutex;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            wchar_t class_name[64]{};
            GetClassNameW(dialog, class_name, ARRAYSIZE(class_name));
            Check(std::wstring(class_name) == L"PulseFileOperationWindow", "extraction uses the real copy/move window class");
            Check(!IsWindowEnabled(owner), "shared extraction progress retains modal owner handling");
            {
                std::lock_guard lock(status_mutex);
                status.phase = ops::ArchivePhase::Extracting;
                status.percent = 42;
                status.total_bytes = 10ull * 1024 * 1024 * 1024;
                status.processed_bytes = status.total_bytes * 42 / 100;
                status.bytes_per_second = 123.4 * 1024 * 1024;
                status.eta_seconds = 49;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (Text(dialog) != l10n::Get(l10n::StringId::ArchiveExtracting) && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(Text(dialog) == l10n::Get(l10n::StringId::ArchiveExtracting), "worker status reaches the shared window");
            {
                std::lock_guard lock(status_mutex);
                status.phase = ops::ArchivePhase::Completed;
                status.percent = 100;
                status.processed_bytes = status.total_bytes;
            }
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, dark, kAccent, done, cancel, [&] {
            std::lock_guard lock(status_mutex);
            return app::ArchiveOperationStatus(status, L"C:\\fixtures\\example.zip", L"C:\\output", cancel.load());
        });
        helper.join();
        Check(completed && !cancel.load() && IsWindowEnabled(owner), "shared window finishes and restores its owner");
    }
    for (int action = 0; action < 3; ++action) {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            if (action == 2) {
                RECT bounds{};
                GetClientRect(dialog, &bounds);
                const auto dpi = compat::WindowDpi(dialog);
                const LPARAM point = MAKELPARAM(bounds.right - MulDiv(35, dpi, 96),
                    bounds.bottom - MulDiv(26, dpi, 96));
                SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
                SendMessageW(dialog, WM_LBUTTONUP, 0, point);
            } else SendMessageW(dialog, action == 1 ? WM_CLOSE : WM_KEYDOWN, action == 1 ? 0 : VK_ESCAPE, 0);
            Check(cancel.load(std::memory_order_acquire) == (action == 2),
                "shared close/Escape keeps extraction running while explicit Cancel interrupts it");
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            Check(IsWindow(dialog) && (action == 2 ? !IsWindowEnabled(owner)
                : !IsWindowVisible(dialog) && IsWindowEnabled(owner)),
                "dismiss releases the owner while Cancel retains it until the worker terminates");
            if (action != 2) EnableWindow(owner, FALSE); // A later modal scope owns the disable now.
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel,
            [&] { return app::ArchiveOperationStatus(progress, L"archive.zip", L"output", cancel.load()); });
        helper.join();
        Check(completed == (action != 2) && done.load() && cancel.load() == (action == 2) &&
            (action == 2 ? IsWindowEnabled(owner) : !IsWindowEnabled(owner)),
            "background completion respects a later modal owner; explicit cancellation restores its own owner");
        EnableWindow(owner, TRUE);
    }
    {
        std::atomic_bool done = false, cancel = false;
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            PostThreadMessageW(thread, WM_QUIT, 17, 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!cancel.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(cancel.load() && IsWindow(dialog), "thread quit cancels extraction while preserving worker lifetime");
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel,
            [&] { return app::ArchiveOperationStatus(progress, L"archive.zip", L"output", cancel.load()); });
        helper.join();
        MSG message{};
        bool got_quit = false;
        const auto deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { got_quit = true; break; }
            DispatchMessageW(&message);
        }
        Check(!completed && got_quit && message.wParam == 17 &&
            IsWindowEnabled(owner), "shared progress preserves WM_QUIT and restores the owner after worker termination");
    }
}
void RunFailureCases(HWND owner, DWORD thread) {
    using namespace pulse;
    ops::ArchiveOperationResult result;
    result.exit_code = 2;
    result.password_state = ops::ArchivePasswordState::Required;
    result.error = L"ERROR: Data Error in encrypted file. Wrong password? : 童欣文.pptx";
    const auto failure = app::ArchiveFailureStatus(result, L"C:\\fixtures\\example.zip", L"C:\\output");
    Check(failure.phase == ops::OpPhase::Failed && !failure.active && failure.failure_hr == S_OK &&
        failure.last_error == l10n::Get(l10n::StringId::ArchivePasswordRetryHint) + L"\n\n" + result.error &&
        failure.last_error.ends_with(result.error) && failure.source_label == L"C:\\fixtures\\example.zip" &&
        failure.destination_label == L"C:\\output" && failure.bytes_per_second == 0 && failure.eta_seconds == 0,
        "archive failure preserves diagnostics and endpoints without treating 7zip exit 2 as a Windows error");
    Check(app::IsArchivePasswordFailure(result) &&
        failure.summary == l10n::Get(l10n::StringId::ArchivePasswordOrDataError),
        "encrypted extraction exit 2 gets a localized password-or-damaged-data summary");
    result.password_state = ops::ArchivePasswordState::NotRequired;
    Check(!app::IsArchivePasswordFailure(result), "unencrypted extraction errors are not mislabeled as password failures");
    result.password_state = ops::ArchivePasswordState::Required;
    result.error.clear();
    Check(app::ArchiveFailureStatus(result, L"archive.zip", L"output").last_error ==
        l10n::Get(l10n::StringId::ArchivePasswordRetryHint),
        "empty encrypted extraction failure still has a localized password-or-data explanation");
    result.password_state = ops::ArchivePasswordState::NotRequired;
    Check(app::ArchiveFailureStatus(result, L"archive.zip", L"output").last_error ==
        l10n::Get(l10n::StringId::ArchiveExtractFailed),
        "empty generic extraction failure retains its localized generic explanation");
    result.password_state = ops::ArchivePasswordState::Required;
    result.error = L"ERROR: Data Error in encrypted file. Wrong password? : 童欣文.pptx";
    for (bool dark : {false, true}) {
        for (bool immediate : {false, true}) {
            std::atomic_bool done = immediate, cancel = false;
            auto helper = Automate(thread, owner, [&](HWND dialog) {
                wchar_t class_name[64]{};
                GetClassNameW(dialog, class_name, ARRAYSIZE(class_name));
                Check(std::wstring(class_name) == L"PulseFileOperationWindow" && !IsWindowEnabled(owner),
                    "extraction failure reuses the existing operation window and modal owner");
                done.store(true, std::memory_order_release);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (Text(dialog) != failure.summary && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                Check(Text(dialog) == failure.summary && IsWindowVisible(dialog),
                    "failed extraction stays visible after the worker completes");
                SendMessageW(dialog, immediate ? WM_KEYDOWN : WM_CLOSE, immediate ? VK_ESCAPE : 0, 0);
            }, [&] { done.store(true); });
            const bool shown = app::ShowArchiveProgressWindow(owner, dark, kAccent, done, cancel, [&] {
                return done.load(std::memory_order_acquire) ? failure
                    : app::ArchiveOperationStatus({}, L"archive.zip", L"output", false);
            });
            helper.join();
            Check(shown && !cancel.load() && IsWindowEnabled(owner),
                "closing a failure restores its owner without issuing another cancellation");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    pulse::compat::EnableDpiAwareness();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse archive dialog test owner",
        WS_OVERLAPPEDWINDOW, 100, 100, 700, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) {
        Check(false, "create isolated test owner");
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    Check(SetWindowSubclass(owner, UiTestProc, 9, 0) != FALSE, "install UI-thread readback dispatch");
    const DWORD thread = GetCurrentThreadId();
    if (argc == 2 && std::string_view(argv[1]) == "--background-only") {
        std::atomic_bool done = false, cancel = false;
        ULONGLONG advance_at = 0;
        constexpr wchar_t updated_title[] = L"Extraction progress advanced";
        auto helper = Automate(thread, owner, [&](HWND dialog) {
            Check(IsWindowVisible(dialog) && IsWindowEnabled(owner),
                "standalone extraction leaves the owner available while progress is visible");
            OnUi(dialog, [&] {
                const auto now = GetTickCount64();
                advance_at = now + 150;
                while (Text(dialog) != updated_title && GetTickCount64() < now + 1500) {
                    MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                    MSG message{};
                    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
                }
                Check(Text(dialog) == updated_title,
                    "background extraction refreshes during another UI message loop");
            });
            done.store(true, std::memory_order_release);
        }, [&] { done.store(true); });
        const bool completed = pulse::app::ShowArchiveProgressWindow(owner, false, kAccent, done, cancel, [&] {
            auto status = pulse::app::ArchiveOperationStatus({}, L"archive.zip", L"output", false);
            if (advance_at && GetTickCount64() >= advance_at) status.summary = updated_title;
            return status;
        }, false);
        helper.join();
        Check(completed && !cancel.load() && IsWindowEnabled(owner), "background extraction completes with its owner enabled");
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        return failures.load() == 0 ? 0 : 1;
    }
    const bool failure_only = argc == 2 && std::string_view(argv[1]) == "--failure-only";
    if (argc == 2 && std::string_view(argv[1]) == "--focus-only") {
        RunFocusCases(owner, thread);
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("Archive focus tests: %d failure(s).\n", failures.load());
        return failures.load() == 0 ? 0 : 1;
    }
    if (failure_only) {
        RunFailureCases(owner, thread);
        pulse::l10n::SetLanguage(L"en-US");
        RunFailureCases(owner, thread);
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("Archive failure dialog tests: %d failure(s).\n", failures.load());
        return failures.load() == 0 ? 0 : 1;
    }
    const bool style_only = argc == 2 && std::string_view(argv[1]) == "--popup-style-only";
    if (style_only) dialog_accent = RGB(0, 120, 212);
    theme_refresh_required = style_only;
    if (style_only) SetTimer(owner, kThemeRefreshTimer, 1, RefreshThemeFixture);
    const bool progress_only = argc == 2 && std::string_view(argv[1]) == "--progress-only";
    if (!progress_only) {
        RunExtractCases(owner, thread);
        RunPasswordCases(owner, thread);
    }
    if (style_only) {
        KillTimer(owner, kThemeRefreshTimer);
        Check(theme_refresh_ran, "UI-thread high-contrast refresh fixture reached the open extraction prompt");
        DestroyWindow(owner);
        if (SUCCEEDED(com)) CoUninitialize();
        return failures.load() == 0 ? 0 : 1;
    }
    RunProgressCases(owner, thread);
    pulse::l10n::SetLanguage(L"en-US");
    RunProgressCases(owner, thread, true);
    DestroyWindow(owner);
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("Archive dialog tests: %d failure(s). Screenshots use actual monitor DPI.\n", failures.load());
    return failures.load() == 0 ? 0 : 1;
}
