// SPDX-License-Identifier: Apache-2.0
#include "../app/native_dialog_takeover.h"
#include "../app/app_commands.h"
#include "../app/app_state.h"
#include "../app/native_file_dialog_adapter.h"
#include "../app/file_picker_places.h"
#include "../app/file_picker_tabs.h"
#include "../app/link_resolve.h"
#include "../common/file_dialog_bridge_bypass.h"
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../common/picker_user_storage.h"
#include "../common/user_storage.h"
#include "../ipc/picker_client.h"
#include "../shell_bridge/file_dialog_wrapper.h"

#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cstdio>
#include <atomic>
#include <bit>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

namespace {
namespace wire = pulse::ipc::picker;
using Microsoft::WRL::ComPtr;
constexpr wchar_t kOpenTitle[] = L"Pulse fixture - window takeover";
constexpr wchar_t kNativeSaveTitle[] = L"Pulse fixture - native save takeover";
constexpr wchar_t kNativeCancelTitle[] = L"Pulse fixture - native save cancel takeover";
constexpr wchar_t kNativeOverwriteTitle[] = L"Pulse fixture - native save overwrite";
constexpr wchar_t kFallbackTitle[] = L"Pulse fixture - native fallback";
constexpr wchar_t kCustomConfirmTitle[] = L"Pulse fixture - custom open confirmation";
constexpr wchar_t kFocusOwnerTitle[] = L"Pulse fixture - picker owner";

enum class NativeCase { Open, OpenFocus, Save, SaveFocus, Overwrite, ExplicitFallback, CustomConfirm, StopActive, SwitchToWindows, TerminateWatcher, CancelFocus };
enum class CancelInput { None, Escape, Close, Button };

const wchar_t* NativeTitle(NativeCase test) {
    switch (test) {
    case NativeCase::Save:
    case NativeCase::SaveFocus: return kNativeSaveTitle;
    case NativeCase::CancelFocus: return kNativeCancelTitle;
    case NativeCase::Overwrite: return kNativeOverwriteTitle;
    case NativeCase::ExplicitFallback: return kFallbackTitle;
    case NativeCase::CustomConfirm: return kCustomConfirmTitle;
    default: return kOpenTitle;
    }
}

HWND Find(const wchar_t* cls, const wchar_t* title, DWORD expected_process = 0) {
    const HWND window = FindWindowW(cls, title);
    DWORD process = 0;
    return window && IsWindowVisible(window) && GetWindowThreadProcessId(window, &process) &&
        (!expected_process || process == expected_process) ? window : nullptr;
}

HWND FindFilename(HWND picker, const wchar_t* expected) {
    for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
         edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
        wchar_t text[MAX_PATH]{};
        DWORD_PTR copied = 0;
        if (SendMessageTimeoutW(edit, WM_GETTEXT, ARRAYSIZE(text), reinterpret_cast<LPARAM>(text),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &copied) && wcscmp(text, expected) == 0) return edit;
    }
    return nullptr;
}

HWND PickerFilenameField(HWND picker) {
    HWND lowest = nullptr;
    LONG bottom = 0;
    for (HWND edit = FindWindowExW(picker, nullptr, L"Edit", nullptr); edit;
         edit = FindWindowExW(picker, edit, L"Edit", nullptr)) {
        RECT bounds{};
        if (IsWindowVisible(edit) && GetWindowRect(edit, &bounds) && (!lowest || bounds.bottom > bottom)) {
            lowest = edit;
            bottom = bounds.bottom;
        }
    }
    return lowest;
}

HWND CreateFocusOwner(const wchar_t* title = kFocusOwnerTitle) {
    const HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"Static", title, WS_OVERLAPPEDWINDOW,
        120, 140, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (owner) SetWindowPos(owner, nullptr, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    return owner;
}

bool ClickPickerButton(HWND picker, int right, int y, bool from_bottom) {
    RECT client{};
    if (!GetClientRect(picker, &client)) return false;
    const int dpi = pulse::compat::WindowDpi(picker);
    const auto button = MAKELPARAM(client.right - MulDiv(right, dpi, 96),
        from_bottom ? client.bottom - MulDiv(y, dpi, 96) : MulDiv(y, dpi, 96));
    return PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, button) &&
        PostMessageW(picker, WM_LBUTTONUP, 0, button);
}

bool CancelPicker(HWND picker, CancelInput input) {
    if (input == CancelInput::Escape) {
        const HWND edit = PickerFilenameField(picker);
        return PostMessageW(edit ? edit : picker, WM_KEYDOWN, VK_ESCAPE, 1) != FALSE;
    }
    return input == CancelInput::Button ? ClickPickerButton(picker, 145, 30, true) :
        ClickPickerButton(picker, 20, 20, false);
}

template<class Check>
bool WaitFor(Check check, DWORD timeout = 10000) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        if (check()) return true;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
    return false;
}

bool ActivateBridgeFixture(HWND window) {
    const auto active = [window] { return GetForegroundWindow() == window; };
    if (active() || (SetForegroundWindow(window) && WaitFor(active, 250))) return true;
    RECT rect{};
    POINT cursor{};
    if (!GetWindowRect(window, &rect) || !GetCursorPos(&cursor) || rect.right - rect.left < 300) return false;
    const POINT hit{rect.left + 150, rect.top + 15};
    constexpr UINT flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE;
    const bool was_topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!was_topmost && !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, flags)) return false;
    if (GetAncestor(WindowFromPoint(hit), GA_ROOT) == window && SetCursorPos(hit.x, hit.y)) {
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(static_cast<UINT>(std::size(click)), click, sizeof(INPUT));
        WaitFor(active, 500);
        SetCursorPos(cursor.x, cursor.y);
    }
    if (!was_topmost) SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    return active();
}

int NativeFixture(const std::wstring& directory, NativeCase test) {
    if (FAILED(OleInitialize(nullptr))) return 2;
    int result = 1;
    const bool check_focus = test == NativeCase::OpenFocus || test == NativeCase::SaveFocus || test == NativeCase::CancelFocus;
    const HWND owner = check_focus ? CreateFocusOwner() : nullptr;
    if (check_focus && !owner) { OleUninitialize(); return 2; }
    {
        const bool save = test == NativeCase::Save || test == NativeCase::SaveFocus ||
            test == NativeCase::CancelFocus || test == NativeCase::Overwrite;
        ComPtr<IFileDialog> native, dialog;
        ComPtr<IShellItem> folder;
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        const HMODULE system_dialog = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto entry = system_dialog ? std::bit_cast<Entry>(GetProcAddress(system_dialog, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        const COMDLG_FILTERSPEC filters[] = {{save ? L"Text document" : L"Text files (*.txt)", L"*.txt"}};
        pulse::shell_bridge::FileDialogBridgeServices services;
        bool host_called = false;
        services.run_bridge = [&](const wire::Message& request, HWND, HANDLE) {
            host_called = true;
            wire::Response reply;
            reply.operation = std::get<wire::Request>(request.body).operation;
            if (test == NativeCase::ExplicitFallback) {
                reply.status = wire::Status::UseNative;
            } else {
                reply.status = wire::Status::Unsupported;
                reply.error_source = wire::ErrorSource::Win32;
                reply.error_code = ERROR_NOT_SUPPORTED;
            }
            return wire::Message{request.request_id, std::move(reply)};
        };
        const auto mode = save ? pulse::shell_bridge::DialogFactory::Save : pulse::shell_bridge::DialogFactory::Open;
        const FILEOPENDIALOGOPTIONS options = FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT |
            (save ? FOS_OVERWRITEPROMPT | FOS_NOREADONLYRETURN | FOS_NOCHANGEDIR : FOS_FILEMUSTEXIST);
        if (entry && SUCCEEDED(entry(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&native))) &&
            SUCCEEDED(pulse::shell_bridge::WrapNativeFileDialog(mode,
                native.Get(), services, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            SUCCEEDED(dialog->SetOptions(options)) &&
            SUCCEEDED(dialog->SetFileTypes(1, filters)) && SUCCEEDED(dialog->SetFolder(folder.Get())) &&
            SUCCEEDED(dialog->SetTitle(NativeTitle(test))) &&
            (!(save || test == NativeCase::CustomConfirm) || SUCCEEDED(dialog->SetOkButtonLabel(L"确认"))) &&
            (!save || SUCCEEDED(dialog->SetDefaultExtension(L"txt"))) &&
            SUCCEEDED(dialog->SetFileName(save ? L"save-draft" : L"sample.txt"))) {
            const HRESULT shown = dialog->Show(owner);
            if (test == NativeCase::ExplicitFallback || test == NativeCase::StopActive ||
                test == NativeCase::SwitchToWindows || test == NativeCase::TerminateWatcher || test == NativeCase::CancelFocus) {
                result = host_called && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : 1;
            } else if (host_called && shown == S_OK) {
                ComPtr<IShellItem> selected;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->GetResult(&selected)) &&
                    SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    const auto expected = directory + (test == NativeCase::Save || test == NativeCase::SaveFocus ?
                        L"\\saved-target.txt" : L"\\sample.txt");
                    result = _wcsicmp(path, expected.c_str()) == 0 ? 0 : 1;
                    CoTaskMemFree(path);
                }
            }
        }
    }
    if (owner && !WaitFor([&] { return !IsWindow(owner); }, 9000)) { DestroyWindow(owner); result = 1; }
    OleUninitialize();
    return result;
}

bool LocalPickerCase(const std::filesystem::path& folder) {
    pulse::storage::OverrideDefaultRootForTesting((folder / L"profile").wstring());
    auto state = std::make_unique<pulse::AppState>();
    const HWND owner = CreateFocusOwner();
    if (!owner) return false;
    SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
    constexpr wchar_t title[] = L"Pulse fixture - internal folder";
    bool observed = false;
    std::jthread driver([&] {
        HWND picker = nullptr;
        observed = WaitFor([&] {
            picker = Find(L"PulseFilePicker", title, GetCurrentProcessId());
            return picker && GetWindow(picker, GW_OWNER) == owner && !IsWindowEnabled(owner);
        }, 4000);
        if (observed && WaitFor([&] {
                if (!IsWindow(picker)) return true;
                PostMessageW(picker, WM_KEYDOWN, VK_RETURN, 1);
                return false;
            }, 4000)) return;
        observed = false;
        if (picker) CancelPicker(picker, CancelInput::Escape);
        PostMessageW(owner, WM_CLOSE, 0, 0);
    });
    auto selected = folder.wstring();
    const bool chosen = pulse::PickFolder(owner, selected, title);
    driver.join();
    const bool passed = observed && chosen && selected == folder.wstring() &&
        IsWindowEnabled(owner) && !FindWindowW(L"PulseFilePicker", title);
    DestroyWindow(owner);
    return passed;
}

int WatchNativeFixture(DWORD source_process) {
    wire::PipeHandle source(OpenProcess(SYNCHRONIZE, FALSE, source_process));
    if (!source) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    pulse::app::NativeDialogTakeover watcher;
    if (!watcher.StartForTesting(source_process) ||
        !WaitFor([&] { return watcher.ReadyForTesting(); }, 4000) ||
        !watcher.ObserveForTesting(Find(L"#32770", kOpenTitle, source_process))) return 1;
    WaitForSingleObject(source.Get(), INFINITE);
    return 0;
}

int BrowserPickerFixture(HWND native, const std::wstring& path, pulse::app::NativeDialogKind kind) {
    pulse::app::NativeFileDialog inspected;
    std::wstring reason;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool recognized = SUCCEEDED(com) && pulse::app::InspectNativeFileDialog(native, inspected, reason);
    if (SUCCEEDED(com)) CoUninitialize();
    if (!recognized || inspected.kind != kind) {
        std::fwprintf(stderr, L"[FAIL] Browser file dialog inspection: %ls\n", reason.c_str());
        return 1;
    }
    const bool save = kind == pulse::app::NativeDialogKind::Save;
    const HWND owner = GetWindow(native, GW_OWNER);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    pulse::app::NativeDialogTakeover watcher;
    bool passed = watcher.StartForTesting(inspected.process_id) &&
        WaitFor([&] { return watcher.ReadyForTesting(); }, 3000) && watcher.ObserveForTesting(native);
    HWND picker = nullptr;
    passed = passed && WaitFor([&] {
        picker = Find(L"PulseFilePicker", inspected.title.c_str(), GetCurrentProcessId());
        return picker && IsWindowVisible(picker) && IsWindowVisible(native);
    }, 4500);
    const bool foreground = !save && passed && owner && (GetForegroundWindow() == picker ||
        (SetForegroundWindow(picker) && WaitFor([&] { return GetForegroundWindow() == picker; }, 750)));
    const auto edit = picker ? PickerFilenameField(picker) : nullptr;
    DWORD_PTR updated = 0;
    passed = passed && edit && SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &updated) && updated && PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
    passed = passed && WaitFor([&] {
        return !IsWindow(picker) && !IsWindowVisible(native) && (save ? std::filesystem::is_regular_file(path) :
            !foreground || GetForegroundWindow() == owner);
    }, 8000);
    if (!save && !foreground) {
        std::puts("[LIMIT] Browser picker did not acquire the initial foreground");
        passed = false;
    }
    watcher.Stop();
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", save ?
        "Browser Save writes the fixture at the chosen path" : "Browser Open gives its window back the foreground");
    return passed ? 0 : 1;
}

bool WatcherCase(const std::wstring& folder, NativeCase test, CancelInput cancel_input = CancelInput::None) {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
    const wchar_t* argument = test == NativeCase::OpenFocus ? L"--native-open-focus" :
        test == NativeCase::SaveFocus ? L"--native-save-focus" :
        test == NativeCase::ExplicitFallback ? L"--native-fallback" :
        test == NativeCase::Save ? L"--native-save" :
        test == NativeCase::Overwrite ? L"--native-overwrite" :
        test == NativeCase::CustomConfirm ? L"--native-custom-confirm" :
        test == NativeCase::StopActive ? L"--native-stop" :
        test == NativeCase::SwitchToWindows ? L"--native-switch" :
        test == NativeCase::CancelFocus ? L"--native-cancel-focus" :
        test == NativeCase::TerminateWatcher ? L"--native-terminate-watcher" : L"--native";
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + argument + L" \"" + folder + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &child)) return false;
    wire::PipeHandle process(child.hProcess), thread(child.hThread);
    pulse::app::NativeDialogTakeover watcher;
    const bool external_watcher = test == NativeCase::TerminateWatcher;
    DWORD watcher_pid = GetCurrentProcessId();
    wire::PipeHandle watcher_process;
    bool success = external_watcher || (watcher.StartForTesting(child.dwProcessId) &&
        WaitFor([&] { return watcher.ReadyForTesting(); }, 4000));
    AllowSetForegroundWindow(child.dwProcessId);
    ResumeThread(thread.Get());
    const auto title = NativeTitle(test);
    HWND native = nullptr;
    if (success) success = WaitFor([&] {
        native = Find(L"#32770", title, child.dwProcessId);
        return native != nullptr;
    });
    if (!success)
        std::printf("[DIAG] watcher ready=%d native dialog=%p\n", watcher.ReadyForTesting(), native);
    const auto native_style = native ? GetWindowLongPtrW(native, GWL_EXSTYLE) : 0;
    if (success) {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        success = SUCCEEDED(com) && WaitFor([&] {
            pulse::app::NativeFileDialog inspected;
            std::wstring reason;
            return pulse::app::InspectNativeFileDialog(native, inspected, reason);
        }, 3000);
        if (SUCCEEDED(com)) CoUninitialize();
        if (success && external_watcher) {
            command = L"\"" + std::wstring(executable) + L"\" --watch-native " + std::to_wstring(child.dwProcessId);
            PROCESS_INFORMATION observer{};
            success = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0,
                nullptr, nullptr, &startup, &observer) != FALSE;
            if (success) {
                wire::PipeHandle observer_thread(observer.hThread);
                watcher_process = wire::PipeHandle(observer.hProcess);
                watcher_pid = observer.dwProcessId;
            }
        } else if (success) {
            success = watcher.ObserveForTesting(native);
        }
    }
    if (success && test == NativeCase::ExplicitFallback) {
        const bool intercepted = WaitFor([&] {
            return Find(L"PulseFilePicker", title, GetCurrentProcessId()) != nullptr;
        }, 1300);
        const HWND cancel = GetDlgItem(native, IDCANCEL);
        success = pulse::dialog_bridge::HasNativeFallback(child.dwProcessId) && !intercepted &&
            cancel && IsWindowEnabled(native) &&
            PostMessageW(native, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(cancel));
    } else if (success) {
        HWND picker = nullptr;
        success = WaitFor([&] {
            picker = Find(L"PulseFilePicker", title, watcher_pid);
            return picker != nullptr;
        });
        if (!success) {
            pulse::app::NativeFileDialog inspected;
            std::wstring reason;
            const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool recognized = SUCCEEDED(com) && pulse::app::InspectNativeFileDialog(native, inspected, reason);
            if (SUCCEEDED(com)) CoUninitialize();
            std::fwprintf(stderr, L"[DIAG] picker absent: native foreground=%d recognized=%d reason=%ls\n",
                GetForegroundWindow() == native, recognized, reason.c_str());
        }
        if (success) {
            success = WaitFor([&] {
                return IsWindowVisible(picker) && IsWindowVisible(native) && IsWindowEnabled(native) &&
                    GetWindowLongPtrW(native, GWL_EXSTYLE) == native_style;
            }, 2000);
            if (!success) std::puts("[DIAG] original dialog did not keep its visible and enabled state");
        }
        if (success && (GetForegroundWindow() == native || GetForegroundWindow() == picker)) {
            success = WaitFor([&] {
                RECT source_bounds{}, picker_bounds{}, overlap{};
                if (!GetWindowRect(native, &source_bounds) || !GetWindowRect(picker, &picker_bounds) ||
                    !IntersectRect(&overlap, &source_bounds, &picker_bounds)) return false;
                const POINT center{(overlap.left + overlap.right) / 2, (overlap.top + overlap.bottom) / 2};
                const HWND hit = WindowFromPoint(center);
                return hit && GetAncestor(hit, GA_ROOT) == picker;
            }, 1500);
            if (!success) std::puts("[DIAG] the overlap still hit the original dialog instead of the picker");
        }
        if (success && test == NativeCase::CancelFocus) {
            const HWND owner = Find(L"Static", kFocusOwnerTitle, child.dwProcessId);
            const bool foreground = GetForegroundWindow() == picker ||
                (SetForegroundWindow(picker) && WaitFor([&] { return GetForegroundWindow() == picker; }, 600));
            success = owner && CancelPicker(picker, cancel_input) && WaitFor([&] {
                return !IsWindow(picker) && !IsWindow(native) && IsWindowEnabled(owner);
            }, 4500);
            success = success && foreground && WaitFor([&] { return GetForegroundWindow() == owner; }, 750);
            if (!foreground) std::puts("[DIAG] Native picker did not acquire the initial foreground");
            if (owner) PostMessageW(owner, WM_CLOSE, 0, 0);
        } else if (success && (external_watcher || test == NativeCase::StopActive || test == NativeCase::SwitchToWindows)) {
            if (test == NativeCase::StopActive) {
                watcher.Stop();
                success = watcher.StoppedForTesting();
            } else if (external_watcher) {
                success = TerminateProcess(watcher_process.Get(), 2) &&
                    WaitForSingleObject(watcher_process.Get(), 2000) == WAIT_OBJECT_0;
            } else {
                const HWND other = CreateFocusOwner();
                const bool switched = other && SetForegroundWindow(other) &&
                    WaitFor([&] { return GetForegroundWindow() == other; }, 800);
                const auto settle = GetTickCount64() + 2700;
                success = WaitFor([&] { return GetTickCount64() >= settle; }, 3300) &&
                    IsWindowVisible(picker) && IsWindowVisible(native);
                if (switched) success = success &&
                    !(GetWindowLongPtrW(picker, GWL_EXSTYLE) & WS_EX_TOPMOST);
                else std::puts("[LIMIT] Fixture could not switch the desktop foreground");
                if (other) DestroyWindow(other);
                RECT client{};
                GetClientRect(picker, &client);
                const int dpi = pulse::compat::WindowDpi(picker);
                const auto button = MAKELPARAM(MulDiv(45, dpi, 96), client.bottom - MulDiv(30, dpi, 96));
                success = success && PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, button) &&
                    PostMessageW(picker, WM_LBUTTONUP, 0, button);
            }
            success = success && WaitFor([&] {
                return !IsWindow(picker) && IsWindowVisible(native) && IsWindowEnabled(native) &&
                    GetWindowLongPtrW(native, GWL_EXSTYLE) == native_style;
            }, 2000);
            const HWND cancel = GetDlgItem(native, IDCANCEL);
            success = success && cancel &&
                PostMessageW(native, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), reinterpret_cast<LPARAM>(cancel));
        } else if (success) {
            const bool check_focus = test == NativeCase::OpenFocus || test == NativeCase::SaveFocus;
            const HWND owner = check_focus ? GetWindow(native, GW_OWNER) : nullptr;
            const bool foreground = owner && (GetForegroundWindow() == picker ||
                (SetForegroundWindow(picker) && WaitFor([&] { return GetForegroundWindow() == picker; }, 600)));
            HWND edit = nullptr;
            const auto path = folder + (test == NativeCase::Save || test == NativeCase::SaveFocus ?
                L"\\saved-target" : L"\\sample.txt");
            DWORD_PTR changed = FALSE;
            success = success && WaitFor([&] {
                edit = PickerFilenameField(picker);
                return edit != nullptr;
            }, 3000) && SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &changed) && changed;
            if (!success) std::puts("[DIAG] picker filename field could not be set");
            if (success && test == NativeCase::Overwrite) {
                HWND prompt = nullptr;
                const bool prompt_visible = WaitFor([&] {
                    if (IsWindow(picker) && IsWindowEnabled(edit)) PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                    prompt = GetWindow(native, GW_ENABLEDPOPUP);
                    return !IsWindow(picker) && IsWindowVisible(native) && prompt && prompt != native && IsWindowVisible(prompt);
                });
                const HWND yes = prompt_visible ? GetDlgItem(prompt, IDYES) : nullptr;
                success = yes && PostMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), reinterpret_cast<LPARAM>(yes));
                if (!success) std::printf("[DIAG] overwrite prompt visible=%d yes=%p native visible=%d enabled=%d picker alive=%d\n",
                    prompt_visible, yes, IsWindowVisible(native), IsWindowEnabled(native), IsWindow(picker));
            }
            success = success && WaitFor([&] {
                if (owner && !IsWindow(picker) && !IsWindow(native)) return true;
                if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) return true;
                if (IsWindowEnabled(edit)) {
                    if (test == NativeCase::SaveFocus) ClickPickerButton(picker, 45, 30, true);
                    else PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                }
                return false;
            }, 6000);
            if (check_focus) {
                success = success && owner && foreground && WaitFor([&] {
                    return IsWindowEnabled(owner) && GetForegroundWindow() == owner;
                }, 750);
                if (!success) std::printf("[DIAG] Native foreground picker=%d owner=%d\n",
                    foreground, owner && GetForegroundWindow() == owner);
                if (owner) PostMessageW(owner, WM_CLOSE, 0, 0);
            }
        }
    }
    DWORD status = 1;
    if (!WaitFor([&] { return WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0; }, 3000)) {
        std::puts("[DIAG] native fixture did not finish");
        TerminateProcess(process.Get(), 2);
    } else {
        GetExitCodeProcess(process.Get(), &status);
        if (status) std::printf("[DIAG] native fixture exit=%lu\n", status);
    }
    if (watcher_process && WaitForSingleObject(watcher_process.Get(), 2000) == WAIT_TIMEOUT)
        TerminateProcess(watcher_process.Get(), 2);
    watcher.Stop();
    const bool stopped = watcher.StoppedForTesting();
    if (!stopped) std::puts("[DIAG] foreground observer did not stop");
    return success && status == 0 && stopped;
}

class DialogEventProbe final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileDialogEvents> {
public:
    bool Candidate() const { return candidate_; }
    bool OnCallerThread() const { return caller_thread_; }
    unsigned Attempts() const { return attempts_; }
    unsigned FolderChanges() const { return folder_changes_; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        caller_thread_ &= GetCurrentThreadId() == thread_;
        ComPtr<IOleWindow> window;
        ComPtr<IFileOpenDialog> open;
        ComPtr<IShellItem> result;
        ComPtr<IShellItemArray> results;
        DWORD count = 0;
        HWND hwnd = nullptr;
        wchar_t class_name[32]{};
        candidate_ &= SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&window))) &&
            SUCCEEDED(window->GetWindow(&hwnd)) && IsWindow(hwnd) &&
            GetClassNameW(hwnd, class_name, ARRAYSIZE(class_name)) && wcscmp(class_name, L"PulseFilePicker") == 0 &&
            SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open))) && SUCCEEDED(dialog->GetResult(&result)) &&
            SUCCEEDED(open->GetResults(&results)) && SUCCEEDED(results->GetCount(&count)) && count == 1;
        return ++attempts_ == 1 ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override {
        caller_thread_ &= GetCurrentThreadId() == thread_;
        ++folder_changes_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return E_NOTIMPL; }
private:
    const DWORD thread_ = GetCurrentThreadId();
    bool candidate_ = true, caller_thread_ = true;
    unsigned attempts_ = 0, folder_changes_ = 0;
};

bool BridgeEventCase(const std::filesystem::path& directory, const std::filesystem::path& pulse,
                     bool expect_fallback = false) {
    const auto* title = expect_fallback ? L"Pulse fixture - bridge host exit" : L"Pulse fixture - bridge callback veto";
    const HMODULE system = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    const auto entry = system ? std::bit_cast<Entry>(GetProcAddress(system, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    ComPtr<IFileOpenDialog> native, dialog;
    ComPtr<IShellItem> folder;
    auto sink = Microsoft::WRL::Make<DialogEventProbe>();
    pulse::shell_bridge::FileDialogBridgeServices services;
    services.executable = pulse.wstring();
    DWORD cookie = 0;
    const COMDLG_FILTERSPEC filters[] = {{L"Text files", L"*.txt"}};
    constexpr auto options = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    if (!entry || !sink || FAILED(entry(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&native))) ||
        FAILED(pulse::shell_bridge::WrapNativeFileDialog(pulse::shell_bridge::DialogFactory::Open,
            native.Get(), services, IID_PPV_ARGS(&dialog))) ||
        FAILED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))) ||
        FAILED(dialog->SetOptions(options)) || FAILED(dialog->SetFolder(folder.Get())) ||
        FAILED(dialog->SetFileTypes(1, filters)) || FAILED(dialog->SetTitle(title)) ||
        FAILED(dialog->SetFileName(L"sample.txt")) || FAILED(dialog->Advise(sink.Get(), &cookie))) return false;
    const HWND owner = CreateFocusOwner();
    if (!owner) return false;
    SetForegroundWindow(owner);
    std::atomic_bool finished{false};
    bool saw_picker = false, saw_native = false;
    std::thread input([&] {
        WaitFor([&] {
            if (finished) return true;
            if (const HWND native_picker = Find(L"#32770", title, GetCurrentProcessId())) {
                saw_native = true;
                PostMessageW(native_picker, WM_CLOSE, 0, 0);
            }
            if (const HWND picker = Find(L"PulseFilePicker", title)) {
                saw_picker = true;
                if (const HWND edit = FindFilename(picker, L"sample.txt"); edit && IsWindowEnabled(edit))
                    PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
            }
            return false;
        }, 12000);
        if (!finished) {
            if (const HWND picker = Find(L"PulseFilePicker", title)) PostMessageW(picker, WM_CLOSE, 0, 0);
            if (const HWND picker = Find(L"#32770", title, GetCurrentProcessId())) PostMessageW(picker, WM_CLOSE, 0, 0);
        }
    });
    const HRESULT shown = dialog->Show(owner);
    finished = true;
    input.join();
    ComPtr<IShellItem> result;
    const bool completed = expect_fallback ?
        shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && saw_native && !saw_picker :
        shown == S_OK && saw_picker && !saw_native && sink->Attempts() == 2 && sink->Candidate() &&
            sink->OnCallerThread() && sink->FolderChanges() && SUCCEEDED(dialog->GetResult(&result));
    const bool passed = completed && SUCCEEDED(dialog->Unadvise(cookie)) && IsWindowEnabled(owner);
    DestroyWindow(owner);
    if (!passed) std::printf("[DIAG] show=%08lx picker=%d native=%d attempts=%u candidate=%d caller=%d folders=%u\n",
        static_cast<unsigned long>(shown), saw_picker, saw_native, sink->Attempts(), sink->Candidate(),
        sink->OnCallerThread(), sink->FolderChanges());
    return passed;
}

enum class BridgeOwnerCase { SameThread, InitiallyDisabled, CrossThread, Implicit, HiddenOwner, SwitchedAway };

bool BridgeCase(const std::wstring& directory, const std::filesystem::path& pulse, wire::Operation operation,
                bool no_test = false, bool cancel_open = false, const wchar_t* open_name = L"sample.txt",
                bool preserve_link = false, CancelInput cancel_input = CancelInput::None,
                bool accept_focus = false, BridgeOwnerCase owner_case = BridgeOwnerCase::SameThread) {
    constexpr wchar_t kOpen[] = L"Pulse fixture - Shell bridge open";
    constexpr wchar_t kSave[] = L"Pulse fixture - Shell bridge save";
    const bool save = operation == wire::Operation::SaveFile;
    const bool folder = operation == wire::Operation::PickFolder;
    const wchar_t* title = save ? kSave : kOpen;
    wire::Message message;
    message.request_id[0] = save ? 2 : 1;
    wire::Request request;
    request.operation = operation;
    request.required_capabilities = wire::DialogLabels | (folder ? wire::Folder :
        wire::Filters | (save ? wire::Save | wire::LocalSaveAccessV1 : wire::Open | wire::LocalOpenAccessV1));
    request.options = folder ? 0 : save ? wire::OverwritePrompt | wire::ValidateLocalSaveAccessV1 | wire::SaveNativeDefaults :
        wire::ValidateLocalOpenAccessV1;
    if (preserve_link) {
        request.options |= wire::NoDereferenceLinks;
        request.required_capabilities |= wire::PreserveLinks;
    }
    if (no_test) request.options |= wire::SaveNoTestFileCreate | wire::SaveRequireExistingParent;
    request.initial_directory.assign(directory.begin(), directory.end());
    const std::wstring filename = folder ? L"" : save ? L"bridge-save" : open_name;
    request.filename.assign(filename.begin(), filename.end());
    if (save) request.default_extension = u"txt";
    if (!folder) request.filters = {{u"All files", u"*.*"}};
    request.label_flags = wire::LabelTitle;
    request.title.assign(title, title + wcslen(title));
    message.body = std::move(request);
    wire::PipeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel) return false;
    const bool check_focus = accept_focus || cancel_input != CancelInput::None;
    const bool initially_disabled = owner_case == BridgeOwnerCase::InitiallyDisabled;
    const bool switched_away = owner_case == BridgeOwnerCase::SwitchedAway;
    const HWND other = initially_disabled || switched_away ? CreateFocusOwner(L"Pulse fixture - unrelated window") : nullptr;
    const HWND owner = check_focus ? CreateFocusOwner() : nullptr;
    if ((check_focus && !owner) || ((initially_disabled || switched_away) && !other)) {
        if (owner) DestroyWindow(owner);
        if (other) DestroyWindow(other);
        return false;
    }
    if (owner && !ActivateBridgeFixture(owner)) {
        std::puts("[DIAG] Could not activate the initiating fixture window");
        DestroyWindow(owner);
        if (other) DestroyWindow(other);
        return false;
    }
    const bool hidden_owner = owner_case == BridgeOwnerCase::HiddenOwner;
    const HWND helper = hidden_owner ? CreateWindowExW(WS_EX_TOOLWINDOW, L"Static", nullptr, WS_POPUP,
        0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (hidden_owner && !helper) { DestroyWindow(owner); return false; }
    if (initially_disabled) EnableWindow(owner, FALSE);
    std::atomic_bool finished{false};
    std::atomic<HWND> observed_picker{nullptr};
    bool saw_picker = false, saw_filename = false, foreground = false, cancel_sent = false, switched = false;
    wire::PipeHandle observed_host;
    std::thread input([&] {
        if (!WaitFor([&] {
                if (finished) return true;
                HWND picker = Find(L"PulseFilePicker", title);
                if (!picker) return false;
                DWORD pid = 0;
                GetWindowThreadProcessId(picker, &pid);
                wire::PipeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
                wchar_t path[32768]{};
                DWORD length = ARRAYSIZE(path);
                if (!process || !QueryFullProcessImageNameW(process.Get(), 0, path, &length) ||
                    _wcsicmp(path, pulse.c_str()) != 0) return false;
                saw_picker = true;
                observed_picker = picker;
                if (owner && !observed_host) {
                    observed_host = std::move(process);
                    foreground = ActivateBridgeFixture(picker);
                    if (switched_away) switched = ActivateBridgeFixture(other);
                    if (!accept_focus) {
                        // A synthetic button press can reactivate a background picker.
                        const bool closing = switched_away ? PostMessageW(picker, WM_CLOSE, 0, 0) != FALSE :
                            CancelPicker(picker, cancel_input);
                        cancel_sent = closing && WaitFor([&] { return !IsWindow(picker); }, 5000);
                        if (!cancel_sent) SetEvent(cancel.Get());
                        return true;
                    }
                }
                if (cancel_open) {
                    observed_host = std::move(process);
                    SetEvent(cancel.Get());
                    return true;
                }
                if (folder) {
                    RECT client{};
                    GetClientRect(picker, &client);
                    const int dpi = static_cast<int>(pulse::compat::WindowDpi(picker));
                    const auto button = MAKELPARAM(client.right - MulDiv(45, dpi, 96),
                        client.bottom - MulDiv(30, dpi, 96));
                    PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, button);
                    PostMessageW(picker, WM_LBUTTONUP, 0, button);
                } else if (const HWND edit = FindFilename(picker, filename.c_str())) {
                    saw_filename = true;
                    if (IsWindowEnabled(edit)) {
                        if (save && accept_focus) ClickPickerButton(picker, 45, 30, true);
                        else PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                    }
                }
                return false;
            }, 12000)) SetEvent(cancel.Get());
    });
    wire::Message response;
    bool enabled_as_expected = false;
    bool result_before_hide = false;
    const auto invoke = [&] {
        const HWND modal_owner = owner_case == BridgeOwnerCase::Implicit ? nullptr : helper ? helper : owner;
        response = wire::RunPickerClient(pulse.wstring(), message, modal_owner, cancel.Get());
        enabled_as_expected = !owner || (IsWindowEnabled(owner) != FALSE) == !initially_disabled;
        result_before_hide = IsWindowVisible(observed_picker.load());
        if (initially_disabled) EnableWindow(owner, TRUE);
        finished = true;
    };
    if (owner_case == BridgeOwnerCase::CrossThread) {
        std::jthread caller(invoke);
        if (!WaitFor([&] { return finished.load(); }, 15000)) SetEvent(cancel.Get());
    } else {
        invoke();
    }
    if (owner) WaitFor([&] { return !IsWindow(observed_picker.load()); }, 2000);
    input.join();
    const auto* result = std::get_if<wire::Response>(&response.body);
    if (owner) {
        DWORD exit_code = STILL_ACTIVE;
        const auto status = accept_focus ? wire::Status::Accepted : wire::Status::Cancelled;
        bool ok = result && result->status == status && enabled_as_expected && observed_host &&
            (!initially_disabled || result_before_hide) &&
            WaitFor([&] { return WaitForSingleObject(observed_host.Get(), 0) == WAIT_OBJECT_0; }, 2000) &&
            GetExitCodeProcess(observed_host.Get(), &exit_code);
        if (accept_focus) {
            const auto expected = directory + L"\\" + filename + (save ? L".txt" : L"");
            ok = ok && result->paths == std::vector<std::u16string>{
                std::u16string(expected.begin(), expected.end())};
        } else ok = ok && cancel_sent;
        ok = ok && foreground && (switched_away ? switched && exit_code == 0 :
            exit_code == 0 || exit_code == wire::kPickerHostReturnForeground) &&
            WaitFor([&] { return GetForegroundWindow() == (switched_away ? other : owner); }, 1000);
        if (!ok) std::printf("[DIAG] bridge submitted=%d cancelled=%d picker foreground=%d switched=%d host exit=%lu owner active=%d status=%u error=%u\n",
            saw_filename, cancel_sent, foreground, switched, exit_code, GetForegroundWindow() == owner,
            result ? static_cast<unsigned>(result->status) : 0, result ? result->error_code : 0);
        DestroyWindow(owner);
        if (other) DestroyWindow(other);
        return ok;
    }
    if (no_test) return result && result->status == wire::Status::Unsupported && !saw_picker &&
        !std::filesystem::exists(directory + L"\\bridge-save.txt");
    if (cancel_open) {
        DWORD exit_code = STILL_ACTIVE;
        return result && result->status == wire::Status::Cancelled && observed_host &&
            WaitForSingleObject(observed_host.Get(), 500) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(observed_host.Get(), &exit_code) && exit_code == 0;
    }
    if (!result || result->status != wire::Status::Accepted || result->paths.size() != 1) {
        std::printf("[DIAG] bridge save=%d picker=%d filename=%d status=%u error=%u\n", save, saw_picker, saw_filename,
            result ? static_cast<unsigned>(result->status) : 0, result ? result->error_code : 0);
        return false;
    }
    const auto& returned = result->paths.front();
    const std::wstring actual(returned.begin(), returned.end());
    const auto expected = folder ? directory : directory +
        (save ? L"\\bridge-save.txt" : preserve_link ? L"\\sample.lnk" : L"\\sample.txt");
    return _wcsicmp(actual.c_str(), expected.c_str()) == 0 &&
        (!save || !std::filesystem::exists(expected));
}

bool CreateFixtureShortcut(const std::filesystem::path& folder) {
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return false;
    bool success = false;
    {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        success = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link->SetPath((folder / L"sample.txt").c_str())) && SUCCEEDED(link.As(&file)) &&
            SUCCEEDED(file->Save((folder / L"sample.lnk").c_str(), TRUE));
    }
    OleUninitialize();
    return success;
}

bool ShortcutLocationCase(const std::filesystem::path& target) {
    using namespace pulse;
    const auto links = target / L"links";
    std::filesystem::create_directory(links);
    if (!CreateFixtureShortcut(target)) return false;
    const auto shortcut = links / L"sample.lnk";
    std::filesystem::rename(target / L"sample.lnk", shortcut);
    fs::DirEntry entry;
    entry.name = L"sample.lnk";
    entry.attrs = FILE_ATTRIBUTE_NORMAL;
    if (!app::ResolveLink(shortcut.wstring(), entry)) return false;
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->appPrefs.persist = state->places.persist = state->searchHistory.persist = false;
    state->window_tabs.EnsureDefault();
    state->pane = state->window_tabs.Active()->FocusedPane();
    auto& tab = *state->pane->ActiveTab();
    tab.current_path = links.wstring();
    tab.snapshot_path = tab.current_path;
    tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>(1, entry));
    tab.loading = false;
    tab.SelectIndices({0});
    state->hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"Static", nullptr, WS_POPUP, 0, 0, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!state->hwnd) return false;
    std::atomic_bool enumerated{false};
    const bool worker = state->worker.Start([&](app::WorkResult result) {
        if (!result.snapshot || result.error || result.cancelled) return;
        enumerated.store(std::any_of(result.snapshot->begin(), result.snapshot->end(),
            [](const auto& row) { return row.name == L"sample.txt"; }));
    });
    if (worker) DispatchMenuCommand(*state, app::CmdOpenShortcutLocation);
    const bool passed = worker && tab.current_path == fs::NormalizePath(target.wstring()) &&
        tab.pending_selected_name == L"sample.txt" && WaitFor([&] { return enumerated.load(); }, 4000);
    state->worker.Stop();
    DestroyWindow(state->hwnd);
    state->hwnd = nullptr;
    return passed;
}

bool SidebarCase(const std::filesystem::path& folder, bool with_tabs) {
    using namespace pulse;
    const auto first = (folder / L"First").wstring(), second = (folder / L"Second").wstring();
    const auto cloud = (folder / L"Cloud").wstring(), network = (folder / L"Network").wstring();
    for (const auto* path : {&first, &second, &cloud, &network}) std::filesystem::create_directory(*path);
    const auto config = (folder / L"config").wstring();
    storage::OverrideDefaultRootForTesting(config);
    std::filesystem::create_directory(config);
    std::ofstream(std::filesystem::path(config) / L"places.json") << R"({
        "quick_access_paths": ["C:\\First pin", "C:\\Second pin"],
        "quick_access_order": ["C:\\Second pin", "C:\\First pin"],
        "networks": [{"name":"Saved share","unc":"\\\\test-server\\test-share"}]
    })";
    const auto places = app::LoadFilePickerPlaces([] { return false; });
    if (places.quick_access.size() < 2 || places.quick_access[0].directory != L"C:\\Second pin" ||
        places.quick_access[1].directory != L"C:\\First pin" || places.network.empty() ||
        places.network.front().directory != L"\\\\test-server\\test-share") {
        std::puts("[DIAG] saved quick access order or network location missing");
        return false;
    }

    const auto opened = (folder / L"Workspace").wstring();
    std::filesystem::create_directory(opened);
    auto state = std::make_unique<AppState>();
    state->window_tabs.NewTab(L"");
    state->window_tabs.NewTab(L"pulse:home");
    state->window_tabs.NewTab(cloud).ActiveFolder()->archive_file = L"example.zip";
    state->window_tabs.NewTab(network).ActiveFolder()->listing_failed = true;
    if (with_tabs) state->window_tabs.NewTab(opened).title = L"打开的项目";
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpszClassName = L"PulsePickerTabsFixture";
    wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        if (message == WM_COPYDATA) {
            const auto* request = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            auto* source = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            return source && request && request->dwData == app::kPickerTabsRequest && app::PublishPickerTabs(*source);
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    };
    RegisterClassW(&wc);
    const HWND source = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, state.get());
    if (!source) return false;
    state->core.windows.Add(source, state.get());
    if (with_tabs) {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        auto command = L"\"" + std::wstring(executable) + L"\" --read-picker-tabs " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(source)) + L" \"" + opened + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        const bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child) != FALSE;
        const wire::PipeHandle process(child.hProcess), thread(child.hThread);
        DWORD code = 1;
        if (!launched || !WaitFor([&] { return WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0; }, 3000) ||
            !GetExitCodeProcess(process.Get(), &code) || code) {
            DestroyWindow(source);
            std::puts("[DIAG] separate picker process could not read the open-tab snapshot");
            return false;
        }
    }

    ui::FilePickerSpec spec;
    spec.title = L"Pulse fixture - picker sidebar";
    spec.software_device = true;
    std::atomic_bool snapshot_ok = false;
    spec.load_places = [&](const auto&) {
        auto tabs = app::ReadPickerTabs(source);
        snapshot_ok = with_tabs ? tabs.size() == 1 && tabs[0].label == L"打开的项目" && tabs[0].directory == opened
            : tabs.empty();
        return ui::PickerPlaces{.onedrive = {{L"OneDrive account", cloud, L"\xE753"}}, .open_tabs = std::move(tabs),
            .quick_access = {{L"First pin", first, L"\xE8B7"}, {L"Second pin", second, L"\xE8B7"}},
            .network = {{L"Saved share", network, L"\xE968"}}};
    };
    bool navigated = false;
    std::thread driver;
    spec.on_ready = [&](HWND window) {
        driver = std::thread([&, window] {
            const double scale = GetDpiForWindow(window) / 96.0;
            const auto at = [=](int x, int y) {
                return MAKELPARAM(std::lround(x * scale), std::lround(y * scale));
            };
            const auto click = [&](int y) {
                const auto point = at(75, y);
                PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
                PostMessageW(window, WM_LBUTTONUP, 0, point);
            };
            const auto key = [&](WPARAM value) { PostMessageW(window, WM_KEYDOWN, value, 1); };
            navigated = WaitFor([&] {
                if (FindFilename(window, with_tabs ? opened.c_str() : first.c_str())) return true;
                click(157); // 100 DIP list origin, 30 DIP account and 12 DIP divider.
                return false;
            }, 3500);
            if (navigated) {
                key(VK_HOME);
                key(VK_RETURN);
                navigated = WaitFor([&] { return FindFilename(window, cloud.c_str()); }, 2000);
            }
            if (navigated && with_tabs) {
                key(VK_DOWN);
                key(VK_RETURN);
                navigated = WaitFor([&] { return FindFilename(window, opened.c_str()); }, 2000);
                if (navigated) {
                    key(VK_DOWN);
                    key(VK_RETURN);
                    navigated = WaitFor([&] { return FindFilename(window, first.c_str()); }, 2000);
                }
            }
            if (navigated) {
                navigated = SetWindowPos(window, nullptr, 0, 0, static_cast<int>(std::lround(590 * scale)),
                    static_cast<int>(std::lround(420 * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
            }
            if (navigated) {
                key(VK_END);
                key(VK_RETURN);
                navigated = WaitFor([&] { return FindFilename(window, network.c_str()); }, 2000);
            }
            PostMessageW(window, WM_CLOSE, 0, 0);
        });
    };
    if (FAILED(OleInitialize(nullptr))) { DestroyWindow(source); return false; }
    ui::FilePickerResult selection;
    const bool picked = ui::ShowFilePicker(nullptr, spec, true, {0.20f, 0.50f, 0.85f, 1.0f}, selection);
    if (driver.joinable()) driver.join();
    OleUninitialize();
    DestroyWindow(source);
    return snapshot_ok && navigated && !picked;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring_view mode = argc == 4 ? argv[1] : L"";
    if (mode == L"--read-picker-tabs") {
        const auto source = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::wcstoull(argv[2], nullptr, 10)));
        const auto tabs = pulse::app::ReadPickerTabs(source);
        return tabs.size() == 1 && tabs[0].label == L"打开的项目" && tabs[0].directory == argv[3] ? 0 : 1;
    }
    if (mode == L"--browser-save-fixture" || mode == L"--browser-open-fixture") {
        wchar_t* end = nullptr;
        const auto handle = std::wcstoull(argv[2], &end, 10);
        return handle && !*end ? BrowserPickerFixture(reinterpret_cast<HWND>(static_cast<UINT_PTR>(handle)), argv[3],
            mode == L"--browser-save-fixture" ? pulse::app::NativeDialogKind::Save : pulse::app::NativeDialogKind::Open) : 2;
    }
    if (argc == 3) {
        const std::wstring_view argument = argv[1];
        if (argument == L"--native") return NativeFixture(argv[2], NativeCase::Open);
        if (argument == L"--native-open-focus") return NativeFixture(argv[2], NativeCase::OpenFocus);
        if (argument == L"--native-save-focus") return NativeFixture(argv[2], NativeCase::SaveFocus);
        if (argument == L"--native-save") return NativeFixture(argv[2], NativeCase::Save);
        if (argument == L"--native-overwrite") return NativeFixture(argv[2], NativeCase::Overwrite);
        if (argument == L"--native-fallback") return NativeFixture(argv[2], NativeCase::ExplicitFallback);
        if (argument == L"--native-custom-confirm") return NativeFixture(argv[2], NativeCase::CustomConfirm);
        if (argument == L"--native-stop") return NativeFixture(argv[2], NativeCase::StopActive);
        if (argument == L"--native-switch") return NativeFixture(argv[2], NativeCase::SwitchToWindows);
        if (argument == L"--native-cancel-focus") return NativeFixture(argv[2], NativeCase::CancelFocus);
        if (argument == L"--native-terminate-watcher") return NativeFixture(argv[2], NativeCase::TerminateWatcher);
        if (argument == L"--watch-native") {
            wchar_t* end = nullptr;
            const DWORD source_process = std::wcstoul(argv[2], &end, 10);
            return source_process && !*end ? WatchNativeFixture(source_process) : 2;
        }
    }
    const bool watcher_only = argc == 3 && wcscmp(argv[2], L"--watcher-only") == 0;
    const bool save_only = argc == 3 && wcscmp(argv[2], L"--watcher-save-only") == 0;
    const bool overwrite_only = argc == 3 && wcscmp(argv[2], L"--watcher-overwrite-only") == 0;
    const bool stop_only = argc == 3 && wcscmp(argv[2], L"--watcher-stop-only") == 0;
    const bool switch_only = argc == 3 && wcscmp(argv[2], L"--watcher-switch-only") == 0;
    const bool terminate_only = argc == 3 && wcscmp(argv[2], L"--watcher-termination-only") == 0;
    const bool fallback_only = argc == 3 && wcscmp(argv[2], L"--fallback-only") == 0;
    const bool custom_open_only = argc == 3 && wcscmp(argv[2], L"--custom-open-only") == 0;
    const bool host_only = argc == 3 && wcscmp(argv[2], L"--host-only") == 0;
    const bool events_only = argc == 3 && wcscmp(argv[2], L"--events-only") == 0;
    const bool local_only = argc == 3 && wcscmp(argv[2], L"--local-only") == 0;
    const bool sidebar_only = argc == 3 && wcscmp(argv[2], L"--sidebar-only") == 0;
    const bool shortcut_location_only = argc == 3 && wcscmp(argv[2], L"--shortcut-location-only") == 0;
    const std::wstring_view focus_argument = argc == 3 ? argv[2] : L"";
    const bool accept_focus_only = focus_argument == L"--accept-focus-only";
    const bool save_focus_only = focus_argument == L"--save-focus-only";
    const bool bridge_focus_only = focus_argument == L"--bridge-focus-only";
    const bool focus_only = focus_argument == L"--cancel-focus-only" || focus_argument == L"--cancel-focus-x" ||
        focus_argument == L"--cancel-focus-esc";
    if (argc != 2 && !watcher_only && !save_only && !overwrite_only && !stop_only && !switch_only && !terminate_only && !fallback_only &&
        !custom_open_only && !host_only && !sidebar_only && !local_only && !focus_only &&
        !accept_focus_only && !save_focus_only && !bridge_focus_only &&
        !shortcut_location_only && !events_only) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto folder = std::filesystem::absolute(argv[1]) /
        (L"picker-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directories(folder)) return 2;
    std::ofstream(folder / L"sample.txt") << "fixture";
    if (events_only) {
        const auto ole = OleInitialize(nullptr);
        const auto executable = std::filesystem::absolute(argv[0]);
        const auto pulse = executable.parent_path() / L"Pulse.exe";
        const bool events = SUCCEEDED(ole) && BridgeEventCase(folder, pulse);
        const bool fallback = SUCCEEDED(ole) && BridgeEventCase(folder, executable, true);
        if (SUCCEEDED(ole)) OleUninitialize();
        std::printf("[%s] Caller-thread OnFileOk reads the candidate, vetoes once and accepts on retry\n", events ? "PASS" : "FAIL");
        std::printf("[%s] Host exit before handshake restores the caller and shows the native picker\n", fallback ? "PASS" : "FAIL");
        return events && fallback ? 0 : 1;
    }
    if (shortcut_location_only) {
        const bool passed = ShortcutLocationCase(folder);
        std::printf("[%s] Open shortcut location navigates to and selects the cached target\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (accept_focus_only) {
        const auto pulse = std::filesystem::absolute(argv[0]).parent_path() / L"Pulse.exe";
        const bool native = WatcherCase(folder.wstring(), NativeCase::OpenFocus);
        std::printf("[%s] Compatibility picker returns the selected file and foreground to its caller\n", native ? "PASS" : "FAIL");
        const bool bridge = BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles,
            false, false, L"sample.txt", false, CancelInput::None, true);
        std::printf("[%s] Bridge picker returns the selected file and foreground to its caller\n", bridge ? "PASS" : "FAIL");
        return native && bridge ? 0 : 1;
    }
    if (save_focus_only || bridge_focus_only) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const auto pulse = std::filesystem::absolute(argv[0]).parent_path() / L"Pulse.exe";
        bool ok = true;
        const auto check = [&](bool passed, const char* description) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
            ok &= passed;
        };
        if (save_focus_only) {
            check(WatcherCase(folder.wstring(), NativeCase::SaveFocus), "Compatibility Save returns focus to its caller");
            check(WatcherCase(folder.wstring(), NativeCase::CancelFocus, CancelInput::Button),
                "Compatibility Cancel returns focus to its caller");
            return ok ? 0 : 1;
        }
        const auto bridge = [&](CancelInput cancel, BridgeOwnerCase owner) {
            return BridgeCase(folder.wstring(), pulse, wire::Operation::SaveFile, false, false,
                L"sample.txt", false, cancel, cancel == CancelInput::None, owner);
        };
        check(bridge(CancelInput::None, BridgeOwnerCase::InitiallyDisabled),
            "Bridge Save restores focus when its caller re-enables the already disabled owner");
        check(bridge(CancelInput::Button, BridgeOwnerCase::InitiallyDisabled),
            "Bridge Cancel restores focus when its caller re-enables the already disabled owner");
        check(bridge(CancelInput::None, BridgeOwnerCase::CrossThread),
            "Bridge Save re-enables and activates an owner on another UI thread");
        check(bridge(CancelInput::Button, BridgeOwnerCase::Implicit),
            "Bridge Cancel returns focus to the initiating window when no owner was specified");
        check(bridge(CancelInput::Button, BridgeOwnerCase::HiddenOwner),
            "Bridge Cancel returns focus to the initiating window when its modal owner is hidden");
        check(bridge(CancelInput::Button, BridgeOwnerCase::SwitchedAway),
            "Bridge Cancel preserves foreground after switching to another window");
        check(bridge(CancelInput::Escape, BridgeOwnerCase::SameThread),
            "Bridge Esc returns focus to its caller");
        check(bridge(CancelInput::Close, BridgeOwnerCase::SameThread),
            "Bridge title-bar close returns focus to its caller");
        return ok ? 0 : 1;
    }
    if (local_only) {
        const auto ole = OleInitialize(nullptr);
        const bool passed = SUCCEEDED(ole) && LocalPickerCase(folder);
        if (SUCCEEDED(ole)) OleUninitialize();
        std::printf("[%s] Internal picker runs in-process, accepts a folder and restores its modal owner\n",
            passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (focus_only) {
        const auto pulse = std::filesystem::absolute(argv[0]).parent_path() / L"Pulse.exe";
        bool ok = true;
        for (const auto input : {CancelInput::Escape, CancelInput::Close}) {
            if (focus_argument == L"--cancel-focus-x" && input != CancelInput::Close) continue;
            if (focus_argument == L"--cancel-focus-esc" && input != CancelInput::Escape) continue;
            const auto button = input == CancelInput::Escape ? "Esc" : "X";
            const bool native = WatcherCase(folder.wstring(), NativeCase::CancelFocus, input);
            std::printf("[%s] Compatibility picker returns cancellation to its caller with %s\n", native ? "PASS" : "FAIL", button);
            const bool bridge = BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles,
                false, false, L"sample.txt", false, input);
            std::printf("[%s] Bridge picker returns cancellation to its caller with %s\n", bridge ? "PASS" : "FAIL", button);
            ok &= native && bridge;
        }
        return ok ? 0 : 1;
    }
    if (save_only || overwrite_only) {
        const bool passed = WatcherCase(folder.wstring(), save_only ? NativeCase::Save : NativeCase::Overwrite) &&
            !std::filesystem::exists(folder / L"saved-target.txt");
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", save_only ?
            "Native Save returns the chosen file with its default extension" :
            "Native Save overwrite prompt becomes visible and can be confirmed");
        return passed ? 0 : 1;
    }
    if (sidebar_only) {
        const bool tabs = SidebarCase(folder, true);
        std::printf("[%s] Current folder tabs appear between OneDrive and Quick Access and navigate by mouse and keyboard\n",
            tabs ? "PASS" : "FAIL");
        const bool empty = SidebarCase(folder, false);
        std::printf("[%s] Special tabs leave no group or gap before Quick Access\n", empty ? "PASS" : "FAIL");
        return tabs && empty ? 0 : 1;
    }
    if (fallback_only) {
        const bool fallback = WatcherCase(folder.wstring(), NativeCase::ExplicitFallback);
        std::printf("[%s] Explicit Windows choice bypasses window takeover\n", fallback ? "PASS" : "FAIL");
        return fallback ? 0 : 1;
    }
    if (stop_only || switch_only || terminate_only) {
        const auto test = stop_only ? NativeCase::StopActive : switch_only ? NativeCase::SwitchToWindows :
            NativeCase::TerminateWatcher;
        const bool restored = WatcherCase(folder.wstring(), test);
        std::printf("[%s] Native dialog remains usable after %s\n", restored ? "PASS" : "FAIL",
            stop_only ? "stopping takeover" : switch_only ? "switching to Windows" : "terminating the watcher process");
        return restored ? 0 : 1;
    }
    bool ok = true;
    if (custom_open_only || (!watcher_only && !host_only)) {
        const bool taken_over = WatcherCase(folder.wstring(), NativeCase::CustomConfirm);
        std::printf("[%s] Open fallback with a custom confirmation uses the verified dialog mode\n",
            taken_over ? "PASS" : "FAIL");
        if (custom_open_only) return taken_over ? 0 : 1;
        ok = taken_over;
    }
    if (!host_only) {
        const bool taken_over = WatcherCase(folder.wstring(), NativeCase::Open);
        std::printf("[%s] Window watcher covers and submits the native Open dialog\n",
            taken_over ? "PASS" : "FAIL");
        ok &= taken_over;
        const bool save = WatcherCase(folder.wstring(), NativeCase::Save) &&
            !std::filesystem::exists(folder / L"saved-target.txt");
        std::printf("[%s] Window watcher covers and submits native Save with a custom button, opaque filter and default extension\n",
            save ? "PASS" : "FAIL");
        ok &= save;
        const bool overwrite = WatcherCase(folder.wstring(), NativeCase::Overwrite);
        std::printf("[%s] Native Save overwrite prompt becomes visible and can be confirmed\n", overwrite ? "PASS" : "FAIL");
        ok &= overwrite;
    }
    if (watcher_only) return ok ? 0 : 1;
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
    const auto pulse = std::filesystem::path(executable).parent_path() / L"Pulse.exe";
    const bool bridge_open = BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles);
    std::printf("[%s] authenticated Shell bridge host opens and returns the exact file\n", bridge_open ? "PASS" : "FAIL");
    const bool bridge_save = BridgeCase(folder.wstring(), pulse, wire::Operation::SaveFile);
    std::printf("[%s] Shell bridge adds the default extension without leaving a new file\n", bridge_save ? "PASS" : "FAIL");
    const bool bridge_no_probe = BridgeCase(folder.wstring(), pulse, wire::Operation::SaveFile, true);
    std::printf("[%s] Save with FOS_NOTESTFILECREATE falls back before showing Pulse\n", bridge_no_probe ? "PASS" : "FAIL");
    const bool bridge_cancel = BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles, false, true);
    std::printf("[%s] External cancel lets the host exit normally\n", bridge_cancel ? "PASS" : "FAIL");
    const bool shortcut = CreateFixtureShortcut(folder);
    const bool link_target = shortcut && BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles,
        false, false, L"sample.lnk");
    std::printf("[%s] Default Open returns the shortcut target\n", link_target ? "PASS" : "FAIL");
    const bool link_preserved = shortcut && BridgeCase(folder.wstring(), pulse, wire::Operation::OpenFiles,
        false, false, L"sample.lnk", true);
    std::printf("[%s] FOS_NODEREFERENCELINKS returns the shortcut itself\n", link_preserved ? "PASS" : "FAIL");
    auto long_folder = folder;
    while (long_folder.native().size() < MAX_PATH + 36) long_folder /= std::wstring(52, L'a');
    const std::filesystem::path long_prefixed(L"\\\\?\\" + long_folder.wstring());
    std::error_code error;
    const bool long_created = std::filesystem::create_directories(long_prefixed, error) &&
        (std::ofstream(long_prefixed / L"sample.txt") << "fixture").good();
    const bool long_file = long_created && BridgeCase(long_folder.wstring(), pulse, wire::Operation::OpenFiles);
    std::printf("[%s] Open validates and returns a file past MAX_PATH\n", long_file ? "PASS" : "FAIL");
    const bool long_directory = long_created && BridgeCase(long_folder.wstring(), pulse, wire::Operation::PickFolder);
    std::printf("[%s] Pick Folder validates and returns a folder past MAX_PATH\n", long_directory ? "PASS" : "FAIL");
    pulse::picker_user_storage::Path share_file;
    const bool unc = pulse::picker_user_storage::Parse(L"\\\\example\\share\\subdir\\file.txt", share_file) &&
        pulse::picker_user_storage::CheckAncestors(share_file, true, [](const auto&) { return DRIVE_REMOTE; },
            [&](const std::wstring& path, DWORD& attrs) -> DWORD {
                if (path.size() <= share_file.root_length) return ERROR_BAD_NETPATH;
                attrs = path == share_file.value ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY;
                return ERROR_SUCCESS;
            }, [] { return false; }) == ERROR_SUCCESS;
    std::printf("[%s] UNC file validation starts below the bare share root\n", unc ? "PASS" : "FAIL");
    return ok && bridge_open && bridge_save && bridge_no_probe && bridge_cancel && unc &&
        link_target && link_preserved && long_file && long_directory ? 0 : 1;
}
