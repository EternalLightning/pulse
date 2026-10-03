#include "../ui/file_operation_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <filesystem>
#include <cstdio>
#include <algorithm>

namespace {
int failures = 0;
UINT_PTR timer = 0;
int action = 0;
int test_dpi = 96;
bool narrow = false;
bool capture_ok = false;
bool valid = true;
UINT cancel_message = 0;
std::wstring output;
BOOL CALLBACK FindDialog(HWND hwnd, LPARAM parameter) {
    wchar_t name[80]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring(name) != L"PulseConfirmWindow") return TRUE;
    *reinterpret_cast<HWND*>(parameter) = hwnd;
    return FALSE;
}
void CALLBACK DriveDialog(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer);
    RECT rect{};
    GetWindowRect(dialog, &rect);
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(test_dpi, test_dpi), reinterpret_cast<LPARAM>(&rect));
    if (narrow) SetWindowPos(dialog, nullptr, 0, 0, 280 * test_dpi / 96, 480 * test_dpi / 96,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(dialog);
    capture_ok = SendMessageW(dialog, pulse::ui::kConfirmSnapshotMessage, 0, reinterpret_cast<LPARAM>(output.c_str())) != 0;
    if (action == 4) {
        pulse::ui::ConfirmDialogInspection info;
        SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
        const float before = info.scroll;
        SendMessageW(dialog, WM_KEYDOWN, VK_END, 0);
        SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&info));
        const bool at_end = info.row_count == 12 && info.max_scroll > 0.0f &&
            info.scroll > before && info.scroll == info.max_scroll &&
            info.content.bottom <= std::min(info.primary.top, info.cancel.top);
        std::printf("[%s] all twelve owner rows remain inspectable at the end of the scroll range\n",
            at_end ? "PASS" : "FAIL");
        if (!at_end) ++failures;
        SendMessageW(dialog, WM_APP + 0x2B1, 0, reinterpret_cast<LPARAM>(output.c_str()));
        SendMessageW(dialog, cancel_message ? cancel_message : WM_CLOSE, cancel_message ? VK_ESCAPE : 0, 0);
    } else if (action == 3) {
        valid = false;
        SendMessageW(dialog, WM_TIMER, 1, 0);
    } else if (action == 0) SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    else {
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        if (action == 2) SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    }
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto directory = argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path(L"bench_data");
    std::filesystem::create_directories(directory);
    ui::LockedItemDialogText text;
    using I = l10n::StringId;
    text.title = l10n::Get(I::LockedItemTitle); text.message = l10n::Get(I::LockedItemMessage);
    text.close_hint = l10n::Get(I::LockedItemCloseHint); text.end_hint = l10n::Get(I::LockedItemEndHint);
    text.retry = l10n::Get(I::LockedItemRetry); text.end_retry = l10n::Get(I::LockedItemEndRetry);
    text.cancel = l10n::Get(I::Cancel);
    ops::OpStatus status;
    status.locked_path = L"C:\\fixture\\长名称的正在占用的项目文件.txt";
    ops::LockOwner owner;
    owner.pid = 4242; owner.app_name = L"Fixture 文本编辑器";
    owner.image_path = L"C:\\fixture\\TextEditor.exe"; owner.can_terminate = true;
    status.lock_owners.push_back(owner);
    for (const bool dark : {false, true}) {
        for (const auto dpi : {96, 120, 144}) {
            for (int choice = 0; choice <= 3; ++choice) {
                action = choice; test_dpi = dpi; narrow = choice == 1; valid = true; capture_ok = false;
                output = (directory / (L"locked_prompt_" + std::to_wstring(dpi) +
                    (dark ? L"_dark_" : L"_light_") + std::to_wstring(choice) + L".png")).wstring();
                timer = SetTimer(nullptr, 0, 250, DriveDialog);
                const auto result = ui::ShowLockedItemDialog(nullptr, status, text, dark,
                    D2D1::ColorF(0x0078D4), [] { return valid; });
                KillTimer(nullptr, timer);
                const auto expected = choice == 1 ? ui::LockedItemChoice::Retry
                    : choice == 2 ? ui::LockedItemChoice::EndAndRetry : ui::LockedItemChoice::Cancel;
                const bool ok = capture_ok && result == expected;
                std::printf("[%s] %s %d percent lock dialog action %d, narrow %d\n",
                    ok ? "PASS" : "FAIL", dark ? "dark" : "light", dpi * 100 / 96, choice, narrow);
                if (!ok) ++failures;
            }
        }
    }
    for (const bool escape : {false, true}) {
        status.lock_owners.clear();
        for (DWORD index = 0; index < 12; ++index) {
            auto process = owner;
            process.pid = 4242 + index;
            process.app_name = L"Owner " + std::to_wstring(index + 1);
            process.image_path = L"C:\\fixture\\" + std::wstring(80, L'长') + L"\\Editor" + std::to_wstring(index + 1) + L".exe";
            status.lock_owners.push_back(std::move(process));
        }
        action = 4; test_dpi = 144; narrow = true; valid = true; capture_ok = false;
        cancel_message = escape ? WM_KEYDOWN : 0;
        output = (directory / (escape ? L"locked_many_escape.png" : L"locked_many_close.png")).wstring();
        timer = SetTimer(nullptr, 0, 250, DriveDialog);
        const auto result = ui::ShowLockedItemDialog(nullptr, status, text, true,
            D2D1::ColorF(0x0078D4), [] { return valid; });
        KillTimer(nullptr, timer);
        const bool ok = capture_ok && result == ui::LockedItemChoice::Cancel;
        std::printf("[%s] complete multi-owner list remains scrollable and %s refuses termination\n",
            ok ? "PASS" : "FAIL", escape ? "Escape" : "window close");
        if (!ok) ++failures;
    }
    cancel_message = 0;
    status.lock_owners = {owner};
    status.lock_owners[0].can_terminate = false;
    action = 1; test_dpi = 96; narrow = false; valid = true; capture_ok = false;
    output = (directory / L"locked_protected_retry.png").wstring();
    timer = SetTimer(nullptr, 0, 250, DriveDialog);
    const auto protected_result = ui::ShowLockedItemDialog(nullptr, status, text, false,
        D2D1::ColorF(0x0078D4), [] { return valid; });
    KillTimer(nullptr, timer);
    const bool protected_ok = capture_ok && protected_result == ui::LockedItemChoice::Retry;
    std::printf("[%s] protected owners permit only ordinary retry\n", protected_ok ? "PASS" : "FAIL");
    if (!protected_ok) ++failures;
    OleUninitialize();
    return failures ? 1 : 0;
}
