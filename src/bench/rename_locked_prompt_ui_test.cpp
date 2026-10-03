#include "../ui/file_operation_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <filesystem>
#include <cstdio>

namespace {
int failures = 0;
UINT_PTR timer_id = 0;
int action = 0;
int dpi = 96;
bool valid = true;
std::wstring output;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
BOOL CALLBACK FindDialog(HWND hwnd, LPARAM value) {
    wchar_t name[80]{}; GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) != L"PulseConfirmWindow") return TRUE;
    *reinterpret_cast<HWND*>(value) = hwnd;
    return FALSE;
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer_id);
    RECT rect{}; GetWindowRect(dialog, &rect);
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    pulse::ui::ConfirmDialogInspection info;
    Check(SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info)) != 0 &&
        info.row_count == 1 && info.alternate.right <= info.alternate.left,
        "rename lock prompt contains a path and no process-termination action");
    Check(SendMessageW(dialog, pulse::ui::kConfirmSnapshotMessage, 0, reinterpret_cast<LPARAM>(output.c_str())) != 0,
        "rename lock prompt saves layout evidence");
    if (action == 1) {
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    } else if (action == 2) SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
    else if (action == 3) SendMessageW(dialog, WM_CLOSE, 0, 0);
    else if (action == 4) { valid = false; SendMessageW(dialog, WM_TIMER, 1, 0); }
    else SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    const auto directory = argc == 2 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(L"bench_data/rename-lock-prompt");
    std::filesystem::create_directories(directory);
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::Initialize(GetModuleHandleW(nullptr), language);
        for (bool dark : {false, true}) {
            for (int resolution : {96, 144}) {
                for (int choice = 0; choice < 5; ++choice) {
                    dpi = resolution; action = choice; valid = true;
                    output = (directory / (std::wstring(language) + (dark ? L"-dark-" : L"-light-") +
                        std::to_wstring(dpi) + L"-" + std::to_wstring(choice) + L".png")).wstring();
                    ops::OpStatus status;
                    status.type = ops::OpType::Rename;
                    status.failed_path = L"E:\\fixture\\" + std::wstring(80, L'长') + L"file.txt";
                    timer_id = SetTimer(nullptr, 0, 200, Drive);
                    const bool retry = ui::ShowRenameLockedDialog(nullptr, status, dark,
                        D2D1::ColorF(0x0078D4), [] { return valid; });
                    KillTimer(nullptr, timer_id);
                    Check(retry == (choice == 1),
                        "rename prompt retries only explicitly and default/Esc/close/stale cancel safely");
                }
            }
        }
    }
    OleUninitialize();
    return failures ? 1 : 0;
}
