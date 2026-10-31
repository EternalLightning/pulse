// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ipc/shell_interaction_client.h"
#include "../ipc/shell_client.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace pulse::ipc;
int failures = 0;
unsigned ticks = 0;
void Check(bool passed, const char* label) {
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!passed) ++failures;
}
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER) { ++ticks; return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

void Protocol() {
    ShellLaunchRequest request;
    request.kind = ShellLaunchKind::PickFile;
    request.flags = 0;
    request.title = L"图片选择";
    request.directory = L"C:\\中文路径";
    request.filters = {L"Images", L"*.jpg;*.png", L"All", L"*.*"};
    ShellLaunchRequest parsed;
    const auto valid = WriteShellLaunch(request);
    Check(ReadShellLaunch(valid.data(), valid.size(), parsed) && parsed.title == request.title &&
        parsed.filters == request.filters && parsed.directory == request.directory,
        "versioned file picker preserves Unicode title, filters and initial folder");
    request.filters.pop_back();
    auto bytes = WriteShellLaunch(request);
    Check(!ReadShellLaunch(bytes.data(), bytes.size(), parsed), "reject odd filter name/pattern pairs");
    request.filters.clear();
    request.title = std::wstring(L"ok\0bad", 6);
    bytes = WriteShellLaunch(request);
    Check(!ReadShellLaunch(bytes.data(), bytes.size(), parsed), "reject embedded NUL in picker title");
    request.title.clear();
    request.paths = {L"C:\\file"};
    bytes = WriteShellLaunch(request);
    Check(!ReadShellLaunch(bytes.data(), bytes.size(), parsed), "reject mixed picker and execute fields");
    bytes = valid;
    bytes.push_back(1);
    Check(!ReadShellLaunch(bytes.data(), bytes.size(), parsed), "reject trailing picker bytes");
    request = {};
    request.kind = ShellLaunchKind::ElevatedWait;
    request.flags = 0;
    request.executable = L"C:\\Program Files\\Pulse\\Pulse.Index.exe";
    request.parameters = L"--configure-volume C: on";
    bytes = WriteShellLaunch(request);
    Check(ReadShellLaunch(bytes.data(), bytes.size(), parsed) && parsed.parameters == request.parameters,
        "elevated wait command preserves quoted arguments without executing elevation");
}

struct WindowSearch {
    const std::wstring* title = nullptr;
    HWND found = nullptr;
    static BOOL CALLBACK Visit(HWND window, LPARAM context) {
        auto& search = *reinterpret_cast<WindowSearch*>(context);
        wchar_t title[512]{};
        GetWindowTextW(window, title, ARRAYSIZE(title));
        if (*search.title == title && IsWindowVisible(window)) {
            search.found = window;
            return FALSE;
        }
        return TRUE;
    }
};

struct EditSearch {
    HWND found = nullptr;
    static BOOL CALLBACK Visit(HWND window, LPARAM context) {
        auto& search = *reinterpret_cast<EditSearch*>(context);
        wchar_t name[64]{};
        GetClassNameW(window, name, ARRAYSIZE(name));
        const int id = GetDlgCtrlID(window);
        if (_wcsicmp(name, L"Edit") == 0 && (id == 1148 || id == 1152)) {
            search.found = window;
            return FALSE;
        }
        return TRUE;
    }
};

enum class Action { Accept, Cancel, Crash };
ShellInteractionResult Dialog(HWND owner, bool folder, const std::filesystem::path& root,
                              Action action, DWORD& helper, bool& controlled) {
    ShellLaunchRequest request;
    request.kind = folder ? ShellLaunchKind::PickFolder : ShellLaunchKind::PickFile;
    request.flags = 0;
    request.owner_hwnd = reinterpret_cast<uintptr_t>(owner);
    request.title = L"Pulse isolated picker " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(GetTickCount64());
    request.directory = root.wstring();
    if (!folder) request.filters = {L"Fixture files", L"*.txt", L"All files", L"*.*"};
    std::thread controller([&] {
        const auto deadline = GetTickCount64() + 10000;
        HWND window = nullptr;
        do {
            WindowSearch search{&request.title};
            EnumWindows(WindowSearch::Visit, reinterpret_cast<LPARAM>(&search));
            window = search.found;
            if (!window) Sleep(20);
        } while (!window && GetTickCount64() < deadline);
        if (!window) return;
        GetWindowThreadProcessId(window, &helper);
        Sleep(150);
        if (action == Action::Crash) {
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, helper);
            if (process) {
                controlled = TerminateProcess(process, ERROR_PROCESS_ABORTED) != FALSE;
                WaitForSingleObject(process, 3000);
                CloseHandle(process);
            } else PostMessageW(window, WM_CLOSE, 0, 0);
        } else if (action == Action::Cancel) {
            controlled = PostMessageW(window, WM_COMMAND, IDCANCEL, 0) != FALSE;
        } else {
            if (!folder) {
                EditSearch search;
                EnumChildWindows(window, EditSearch::Visit, reinterpret_cast<LPARAM>(&search));
                DWORD_PTR ignored = 0;
                controlled = search.found && SendMessageTimeoutW(search.found, WM_SETTEXT, 0,
                    reinterpret_cast<LPARAM>(L"中文文件.txt"), SMTO_ABORTIFHUNG, 2000, &ignored);
                if (!controlled) { PostMessageW(window, WM_COMMAND, IDCANCEL, 0); return; }
            }
            controlled = PostMessageW(window, WM_COMMAND, IDOK, 0) != FALSE;
            const auto close_deadline = GetTickCount64() + 8000;
            while (IsWindow(window) && GetTickCount64() < close_deadline) Sleep(20);
            if (IsWindow(window)) {
                controlled = false;
                PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
            }
        }
    });
    auto result = RunShellInteraction(request);
    controller.join();
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    Protocol();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--protocol") return failures ? 1 : 0;
    std::error_code error;
    const auto root = std::filesystem::current_path() / "bench_data" /
        ("shell-picker-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root, error);
    std::ofstream(root / L"中文文件.txt") << "isolated fixture";
    WNDCLASSW cls{};
    cls.lpfnWndProc = OwnerProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseShellPickerTestOwner";
    RegisterClassW(&cls);
    HWND owner = CreateWindowW(cls.lpszClassName, L"Pulse Shell picker test", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, cls.hInstance, nullptr);
    Check(owner && !error, "create isolated picker fixture and owner window");
    if (!owner || error) return 1;
    SetTimer(owner, 1, 25, nullptr);
    ShellClient::Instance().Start({});
    DWORD helper = 0;
    bool controlled = false;
    const auto file = Dialog(owner, false, root, Action::Accept, helper, controlled);
    Check(controlled && helper && helper != GetCurrentProcessId() && SUCCEEDED(file.error) &&
        file.value == (root / L"中文文件.txt").wstring(), "real file picker returns Unicode path from helper process");
    Check(ticks > 0 && IsWindowEnabled(owner), "modal wait dispatches owner timer and restores enabled state");
    helper = 0; controlled = false;
    const auto folder = Dialog(owner, true, root, Action::Accept, helper, controlled);
    Check(controlled && helper != GetCurrentProcessId() && SUCCEEDED(folder.error) &&
        folder.value == root.wstring(), "real folder picker accepts requested initial directory");
    helper = 0; controlled = false;
    const auto cancelled = Dialog(owner, true, root, Action::Cancel, helper, controlled);
    Check(controlled && cancelled.cancelled && cancelled.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
        IsWindowEnabled(owner), "picker cancellation restores owner without publishing a selection");
    helper = 0; controlled = false;
    const auto crashed = Dialog(owner, true, root, Action::Crash, helper, controlled);
    Check(controlled && crashed.error == kShellLaunchOutcomeUnknown && IsWindowEnabled(owner),
        "crashing visible dialog helper preserves client and restores modal owner");
    helper = 0; controlled = false;
    const auto recovered = Dialog(owner, true, root, Action::Cancel, helper, controlled);
    Check(controlled && recovered.cancelled, "new dialog session works after helper crash");
    ShellClient::Instance().Stop();
    KillTimer(owner, 1);
    DestroyWindow(owner);
    std::filesystem::remove_all(root, error);
    Check(!error && !std::filesystem::exists(root), "isolated picker fixture cleanup");
    return failures ? 1 : 0;
}
