// SPDX-License-Identifier: Apache-2.0
#include "../app/native_dialog_takeover.h"
#include "../app/native_file_dialog_adapter.h"
#include "../common/file_dialog_bridge_bypass.h"
#include "../common/localization.h"
#include "../ipc/picker_client.h"

#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <atomic>
#include <bit>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
namespace wire = pulse::ipc::picker;
using Microsoft::WRL::ComPtr;
constexpr wchar_t kOpenTitle[] = L"Pulse fixture - window takeover";
constexpr wchar_t kFallbackTitle[] = L"Pulse fixture - native fallback";

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

int NativeFixture(const std::wstring& directory, bool fallback) {
    if (FAILED(OleInitialize(nullptr))) return 2;
    int result = 1;
    {
        ComPtr<IFileOpenDialog> dialog;
        ComPtr<IShellItem> folder;
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        const HMODULE system_dialog = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto entry = system_dialog ? std::bit_cast<Entry>(GetProcAddress(system_dialog, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        const COMDLG_FILTERSPEC filters[] = {{L"Text files (*.txt)", L"*.txt"}};
        if (entry && SUCCEEDED(entry(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            SUCCEEDED(dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT)) &&
            SUCCEEDED(dialog->SetFileTypes(1, filters)) && SUCCEEDED(dialog->SetFolder(folder.Get())) &&
            SUCCEEDED(dialog->SetTitle(fallback ? kFallbackTitle : kOpenTitle)) &&
            SUCCEEDED(dialog->SetFileName(L"sample.txt"))) {
            if (fallback) {
                pulse::dialog_bridge::NativeFallbackScope scope;
                result = dialog->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : 1;
            } else if (dialog->Show(nullptr) == S_OK) {
                ComPtr<IShellItem> selected;
                PWSTR path = nullptr;
                if (SUCCEEDED(dialog->GetResult(&selected)) &&
                    SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = _wcsicmp(path, (directory + L"\\sample.txt").c_str()) == 0 ? 0 : 1;
                    CoTaskMemFree(path);
                }
            }
        }
    }
    OleUninitialize();
    return result;
}

bool WatcherCase(const std::wstring& folder, bool fallback) {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " +
        (fallback ? L"--native-fallback" : L"--native") + L" \"" + folder + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &child)) return false;
    wire::PipeHandle process(child.hProcess), thread(child.hThread);
    pulse::app::NativeDialogTakeover watcher;
    bool success = watcher.StartForTesting(child.dwProcessId) &&
        WaitFor([&] { return watcher.ReadyForTesting(); }, 4000);
    AllowSetForegroundWindow(child.dwProcessId);
    ResumeThread(thread.Get());
    const auto title = fallback ? kFallbackTitle : kOpenTitle;
    HWND native = nullptr;
    if (success) success = WaitFor([&] {
        native = Find(L"#32770", title, child.dwProcessId);
        return native != nullptr;
    });
    if (!success)
        std::printf("[DIAG] watcher ready=%d native dialog=%p\n", watcher.ReadyForTesting(), native);
    if (success) {
        SetForegroundWindow(native);
        if (GetForegroundWindow() != native) {
            std::puts("[LIMIT] desktop denied foreground activation; exercising the observed fixture HWND directly");
            success = watcher.ObserveForTesting(native);
        }
    }
    if (success && fallback) {
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
            picker = Find(L"PulseFilePicker", title, GetCurrentProcessId());
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
            HWND edit = nullptr;
            const auto path = folder + L"\\sample.txt";
            DWORD_PTR changed = FALSE;
            success = WaitFor([&] {
                edit = FindFilename(picker, L"sample.txt");
                return edit != nullptr;
            }, 3000) && SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &changed) && changed;
            if (!success) std::puts("[DIAG] picker filename field could not be set");
            success = success && WaitFor([&] {
                if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) return true;
                if (IsWindowEnabled(edit)) PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                return false;
            }, 6000);
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
    watcher.Stop();
    const bool stopped = WaitFor([&] { return watcher.StoppedForTesting(); }, 5000);
    if (!stopped) std::puts("[DIAG] foreground observer did not stop");
    return success && status == 0 && stopped;
}

bool BridgeCase(const std::wstring& directory, const std::filesystem::path& pulse, bool save) {
    constexpr wchar_t kOpen[] = L"Pulse fixture - Shell bridge open";
    constexpr wchar_t kSave[] = L"Pulse fixture - Shell bridge save";
    const wchar_t* title = save ? kSave : kOpen;
    wire::Message message;
    message.request_id[0] = save ? 2 : 1;
    wire::Request request;
    request.operation = save ? wire::Operation::SaveFile : wire::Operation::OpenFiles;
    request.required_capabilities = (save ? wire::Save | wire::LocalSaveAccessV1 : wire::Open | wire::LocalOpenAccessV1) |
        wire::Filters | wire::DialogLabels;
    request.options = save ? wire::OverwritePrompt | wire::ValidateLocalSaveAccessV1 | wire::SaveNativeDefaults :
        wire::ValidateLocalOpenAccessV1;
    request.initial_directory.assign(directory.begin(), directory.end());
    request.filename = save ? u"bridge-save" : u"sample.txt";
    if (save) request.default_extension = u"txt";
    request.filters = {{u"Text files", u"*.txt"}};
    request.label_flags = wire::LabelTitle;
    request.title.assign(title, title + wcslen(title));
    message.body = std::move(request);
    wire::PipeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel) return false;
    std::atomic_bool finished{false};
    bool saw_picker = false, saw_filename = false;
    std::thread input([&] {
        if (!WaitFor([&] {
                if (finished) return true;
                HWND picker = Find(L"PulseFilePicker", title);
                if (!picker) return false;
                DWORD pid = 0;
                GetWindowThreadProcessId(picker, &pid);
                wire::PipeHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
                wchar_t path[32768]{};
                DWORD length = ARRAYSIZE(path);
                if (!process || !QueryFullProcessImageNameW(process.Get(), 0, path, &length) ||
                    _wcsicmp(path, pulse.c_str()) != 0) return false;
                saw_picker = true;
                const HWND filename = FindFilename(picker, save ? L"bridge-save" : L"sample.txt");
                saw_filename |= filename != nullptr;
                if (filename && IsWindowEnabled(filename)) PostMessageW(filename, WM_KEYDOWN, VK_RETURN, 1);
                return false;
            }, 12000)) SetEvent(cancel.Get());
    });
    const auto response = wire::RunPickerClient(pulse.wstring(), message, nullptr, cancel.Get());
    finished = true;
    input.join();
    const auto* result = std::get_if<wire::Response>(&response.body);
    if (!result || result->status != wire::Status::Accepted || result->paths.size() != 1) {
        std::printf("[DIAG] bridge save=%d picker=%d filename=%d status=%u error=%u\n", save, saw_picker, saw_filename,
            result ? static_cast<unsigned>(result->status) : 0, result ? result->error_code : 0);
        return false;
    }
    const auto& returned = result->paths.front();
    const std::wstring actual(returned.begin(), returned.end());
    const auto expected = directory + (save ? L"\\bridge-save.txt" : L"\\sample.txt");
    return _wcsicmp(actual.c_str(), expected.c_str()) == 0 &&
        (!save || !std::filesystem::exists(expected));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && (wcscmp(argv[1], L"--native") == 0 || wcscmp(argv[1], L"--native-fallback") == 0))
        return NativeFixture(argv[2], wcscmp(argv[1], L"--native-fallback") == 0);
    const bool watcher_only = argc == 3 && wcscmp(argv[2], L"--watcher-only") == 0;
    const bool fallback_only = argc == 3 && wcscmp(argv[2], L"--fallback-only") == 0;
    const bool host_only = argc == 3 && wcscmp(argv[2], L"--host-only") == 0;
    if (argc != 2 && !watcher_only && !fallback_only && !host_only) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto folder = std::filesystem::absolute(argv[1]) /
        (L"picker-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directories(folder)) return 2;
    std::ofstream(folder / L"sample.txt") << "fixture";
    if (fallback_only) {
        const bool fallback = WatcherCase(folder.wstring(), true);
        std::printf("[%s] Shell bridge fallback marker preserves the Windows dialog\n", fallback ? "PASS" : "FAIL");
        return fallback ? 0 : 1;
    }
    bool ok = true;
    if (!host_only) {
        ok = WatcherCase(folder.wstring(), false);
        std::printf("[%s] Window watcher submits to the real Windows Open dialog\n", ok ? "PASS" : "FAIL");
    }
    if (watcher_only) return ok ? 0 : 1;
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
    const auto pulse = std::filesystem::path(executable).parent_path() / L"Pulse.exe";
    const bool bridge_open = BridgeCase(folder.wstring(), pulse, false);
    std::printf("[%s] authenticated Shell bridge host opens and returns the exact file\n", bridge_open ? "PASS" : "FAIL");
    const bool bridge_save = BridgeCase(folder.wstring(), pulse, true);
    std::printf("[%s] Shell bridge adds the default extension without leaving a new file\n", bridge_save ? "PASS" : "FAIL");
    return ok && bridge_open && bridge_save ? 0 : 1;
}
