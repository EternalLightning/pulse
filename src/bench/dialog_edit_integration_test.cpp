#include "../ui/advanced_search_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
UINT_PTR timer_id = 0;
std::wstring dialog_class;
int failures = 0;
int driven = 0;
int dpi = 96;
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
void CALLBACK DriveDialog(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer_id);
    ++driven;
    RECT rect{};
    GetWindowRect(dialog, &rect);
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    std::vector<HWND> fields;
    EnumChildWindows(dialog, FindEdit, reinterpret_cast<LPARAM>(&fields));
    Check(fields.size() >= 3, "real dialog exposes its native input fields");
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

int wmain() {
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    for (bool fallback : {false, true}) {
        SetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", fallback ? L"1" : nullptr);
        for (bool dark : {false, true}) {
            for (int resolution : {96, 144}) {
                dpi = resolution;
                dialog_class = L"PulseAdvancedSearchWindow";
                timer_id = SetTimer(nullptr, 0, 200, DriveDialog);
                app::AdvancedSearchSpec spec;
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
    Check(driven == 16, "both dialogs are exercised in normal and native fallback at both themes and DPIs");
    OleUninitialize();
    return failures ? 1 : 0;
}
