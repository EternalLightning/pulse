// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/advanced_search_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/typography.h"
#include <commctrl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {
UINT_PTR timer_id = 0;
std::wstring dialog_class;
int failures = 0;
int driven = 0;
int dpi = 96;
bool style_only = false;
bool dark_theme = false;
const auto output = std::filesystem::absolute(L"bench_data/dialog-style");
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
BOOL CALLBACK FindDialog(HWND hwnd, LPARAM parameter) {
    wchar_t name[80]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (dialog_class != name) return TRUE;
    *reinterpret_cast<HWND*>(parameter) = hwnd;
    return FALSE;
}
BOOL CALLBACK FindEdit(HWND hwnd, LPARAM parameter) {
    wchar_t name[32]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) == L"Edit" && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd))
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(hwnd);
    return TRUE;
}
LPARAM Point(float x, float y) {
    return MAKELPARAM(static_cast<int>(std::lround(x * dpi / 96)), static_cast<int>(std::lround(y * dpi / 96)));
}
std::vector<char> Snapshot(HWND dialog, const std::wstring& name) {
    const auto path = output / (name + (dark_theme ? L"-dark-" : L"-light-") + std::to_wstring(dpi) + L".png");
    Check(SendMessageW(dialog, WM_APP + 0x2B5, 0, reinterpret_cast<LPARAM>(path.c_str())) != 0,
        "production modal renderer saves the requested state");
    std::ifstream image(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(image), std::istreambuf_iterator<char>());
}
void ShiftTab(HWND dialog) {
    BYTE keys[256]{};
    GetKeyboardState(keys);
    const BYTE previous = keys[VK_SHIFT];
    keys[VK_SHIFT] = 0x80;
    SetKeyboardState(keys);
    SendMessageW(GetFocus() ? GetFocus() : dialog, WM_KEYDOWN, VK_TAB, 0);
    keys[VK_SHIFT] = previous;
    SetKeyboardState(keys);
}
void DriveStyle(HWND dialog, const std::vector<HWND>& fields) {
    const bool batch = dialog_class == L"PulseBatchRenameWindow";
    const float scale = dpi / 96.0f;
    if (batch) {
        Check((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_THICKFRAME) != 0,
            "batch rename retains interactive resize support");
        RECT before{};
        GetClientRect(dialog, &before);
        const auto first = *std::min_element(fields.begin(), fields.end(), [](HWND a, HWND b) {
            RECT x{}, y{}; GetWindowRect(a, &x); GetWindowRect(b, &y);
            return x.top != y.top ? x.top < y.top : x.left < y.left;
        });
        RECT input_before{};
        GetWindowRect(first, &input_before);
        const int dw = static_cast<int>(160 * scale), dh = static_cast<int>(120 * scale);
        SetWindowPos(dialog, nullptr, 0, 0, before.right + dw, before.bottom + dh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT resized{}, input_after{};
        GetClientRect(dialog, &resized);
        GetWindowRect(first, &input_after);
        Check(resized.right == before.right + dw && resized.bottom == before.bottom + dh &&
            input_after.right - input_after.left > input_before.right - input_before.left,
            "growing batch rename expands its actual client and hosted native input columns");
        POINT close{resized.right - static_cast<int>(23 * scale), static_cast<int>(22 * scale)};
        ClientToScreen(dialog, &close);
        POINT old_close{before.right - static_cast<int>(23 * scale), static_cast<int>(22 * scale)};
        ClientToScreen(dialog, &old_close);
        Check(SendMessageW(dialog, WM_NCHITTEST, 0, MAKELPARAM(close.x, close.y)) == HTCLIENT &&
            SendMessageW(dialog, WM_NCHITTEST, 0, MAKELPARAM(old_close.x, old_close.y)) == HTCAPTION,
            "close hit target follows the resized right edge and its old position becomes draggable title bar");
        SetFocus(first);
        ShiftTab(dialog);
        Snapshot(dialog, L"batch-resized-close-focus");
        const auto image = Snapshot(dialog, L"batch-resized");
        Check(!image.empty(), "resized preview and footer have production snapshot evidence");
        const auto cancel_point = MAKELPARAM(resized.right - static_cast<int>(156 * scale), resized.bottom - static_cast<int>(32 * scale));
        SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, cancel_point);
        SendMessageW(dialog, WM_LBUTTONUP, 0, cancel_point);
        Check(!IsWindow(dialog), "batch cancel hit target follows the resized bottom-right footer");
        return;
    }
    SetFocus(dialog);
    SendMessageW(dialog, WM_MOUSELEAVE, 0, 0);
    auto pressed_feedback = [&](float x, float y, const wchar_t* name) {
        const auto baseline = Snapshot(dialog, std::wstring(name) + L"-normal");
        SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, Point(x, y));
        const auto pressed = Snapshot(dialog, std::wstring(name) + L"-pressed");
        Check(GetCapture() == dialog && !baseline.empty() && baseline != pressed,
            "advanced-search control press changes its production-rendered pixels before activation");
        SendMessageW(dialog, WM_LBUTTONUP, 0, Point(0, 0));
    };
    pressed_feedback(450, 98, L"advanced-toggle");
    pressed_feedback(80, 382, L"advanced-checkbox");
    pressed_feedback(100, 484, L"advanced-reset");
    pressed_feedback(404, 484, L"advanced-cancel");
    pressed_feedback(496, 484, L"advanced-search");
    const auto unfocused = Snapshot(dialog, L"advanced-close-normal");
    ShiftTab(dialog);
    const auto close_focus = Snapshot(dialog, L"advanced-close-focus");
    Check(GetFocus() == dialog && !unfocused.empty() && unfocused != close_focus,
        "backward Tab reaches close and draws a visible keyboard focus indicator");
    ShiftTab(dialog);
    Snapshot(dialog, L"advanced-search-focus");
    ShiftTab(dialog);
    Snapshot(dialog, L"advanced-cancel-focus");
    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
}
void CALLBACK DriveDialog(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer_id);
    ++driven;
    RECT rect{};
    GetWindowRect(dialog, &rect);
    if (style_only) {
        rect.right = rect.left + (dialog_class == L"PulseBatchRenameWindow" ? 620 : 560) * dpi / 96;
        rect.bottom = rect.top + (dialog_class == L"PulseBatchRenameWindow" ? 588 : 516) * dpi / 96;
    }
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    std::vector<HWND> fields;
    EnumChildWindows(dialog, FindEdit, reinterpret_cast<LPARAM>(&fields));
    Check(fields.size() >= 3, "real dialog exposes its native input fields");
    if (style_only) {
        if (!fields.empty()) DriveStyle(dialog, fields);
        if (IsWindow(dialog)) SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        return;
    }
    for (HWND field : fields) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            SetFocus(dialog);
            SendMessageW(field, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
            SendMessageW(field, WM_LBUTTONUP, 0, MAKELPARAM(8, 8));
            Check(GetFocus() == field, "a blurred dialog field accepts repeated mouse focus");
            SendMessageW(field, EM_SETSEL, 0, -1);
            const bool numeric = (GetWindowLongPtrW(field, GWL_STYLE) & ES_NUMBER) != 0;
            SendMessageW(field, WM_CHAR, numeric ? L'7' : L'文', 0);
            SendMessageW(field, WM_CHAR, numeric ? L'2' : L'x', 0);
            wchar_t text[64]{};
            GetWindowTextW(field, text, ARRAYSIZE(text));
            Check(std::wstring(text) == (numeric ? L"72" : L"文x"),
                "real dialog field retains typed Unicode or numeric text");
            if (repeat == 0 && !numeric) {
                SendMessageW(field, WM_KEYDOWN, VK_TAB, 0);
                Check(GetFocus() != field && IsChild(dialog, GetFocus()),
                    "dialog-owned Tab moves focus to another native field");
            }
        }
    }
    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
}
}

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--dialog-style-only") style_only = true;
        else if (std::wstring_view(argv[i]) == L"--sharp")
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Sharp);
        else if (std::wstring_view(argv[i]) == L"--smooth")
            pulse::ui::typography::SetTextRenderMode(pulse::ui::typography::TextRenderMode::Smooth);
    }
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (style_only) std::filesystem::create_directories(output);
    for (bool fallback : {false, true}) {
        if (style_only && fallback) continue;
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", fallback ? L"1" : nullptr);
        for (bool dark : {false, true}) {
            dark_theme = dark;
            for (int resolution : {96, 144}) {
                dpi = resolution;
                dialog_class = L"PulseAdvancedSearchWindow";
                timer_id = SetTimer(nullptr, 0, 200, DriveDialog);
                model::AdvancedSearchSpec spec;
                spec.kind = index::SearchKind::Custom;
                spec.custom_exts = L"txt";
                const auto search = ui::ShowAdvancedSearchDialog(nullptr, spec, dark, D2D1::ColorF(0x0078D4));
                KillTimer(nullptr, timer_id);
                Check(!search.accepted, "real advanced-search editing test cancels without submitting a search");
                dialog_class = L"PulseBatchRenameWindow";
                timer_id = SetTimer(nullptr, 0, 200, DriveDialog);
                const auto rename = ui::ShowBatchRenameDialog(nullptr,
                    {L"C:\\Pulse-isolated-dialog-fixture\\example.txt"}, dark, D2D1::ColorF(0x0078D4));
                KillTimer(nullptr, timer_id);
                Check(!rename.accepted, "real batch-rename editing test cancels without touching files");
            }
        }
    }
    SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", nullptr);
    Check(driven == (style_only ? 8 : 16), "requested real-dialog cases are exercised at both themes and DPIs");
    OleUninitialize();
    return failures ? 1 : 0;
}
