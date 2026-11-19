// SPDX-License-Identifier: Apache-2.0
#include "../app/native_dialog_takeover.h"
#include "../app/native_file_dialog_adapter.h"
#include "../app/file_picker_places.h"
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
constexpr wchar_t kFallbackTitle[] = L"Pulse fixture - native fallback";
constexpr wchar_t kCustomConfirmTitle[] = L"Pulse fixture - custom open confirmation";

enum class NativeCase { Open, ExplicitFallback, CustomConfirm, StopActive, SwitchToWindows, TerminateWatcher };

const wchar_t* NativeTitle(NativeCase test) {
    switch (test) {
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

bool AboveSource(HWND picker, HWND source) {
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (window == picker) return true;
        if (window == source) return false;
    }
    return false;
}

bool CoversSource(HWND picker, HWND source) {
    RECT picker_bounds{}, source_bounds{}, visible_source{}, covered{};
    MONITORINFO monitor{sizeof(monitor)};
    return GetWindowRect(picker, &picker_bounds) && GetWindowRect(source, &source_bounds) &&
        GetMonitorInfoW(MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST), &monitor) &&
        IntersectRect(&visible_source, &source_bounds, &monitor.rcWork) &&
        IntersectRect(&covered, &picker_bounds, &visible_source) && EqualRect(&covered, &visible_source);
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

int NativeFixture(const std::wstring& directory, NativeCase test) {
    if (FAILED(OleInitialize(nullptr))) return 2;
    int result = 1;
    {
        ComPtr<IFileOpenDialog> native, dialog;
        ComPtr<IShellItem> folder;
        using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        const HMODULE system_dialog = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto entry = system_dialog ? std::bit_cast<Entry>(GetProcAddress(system_dialog, "DllGetClassObject")) : nullptr;
        ComPtr<IClassFactory> factory;
        const COMDLG_FILTERSPEC filters[] = {{L"Text files (*.txt)", L"*.txt"}};
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
        if (entry && SUCCEEDED(entry(CLSID_FileOpenDialog, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&native))) &&
            SUCCEEDED(pulse::shell_bridge::WrapNativeFileDialog(pulse::shell_bridge::DialogFactory::Open,
                native.Get(), services, IID_PPV_ARGS(&dialog))) &&
            SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
            SUCCEEDED(dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT)) &&
            SUCCEEDED(dialog->SetFileTypes(1, filters)) && SUCCEEDED(dialog->SetFolder(folder.Get())) &&
            SUCCEEDED(dialog->SetTitle(NativeTitle(test))) &&
            (test != NativeCase::CustomConfirm || SUCCEEDED(dialog->SetOkButtonLabel(L"确认"))) &&
            SUCCEEDED(dialog->SetFileName(L"sample.txt"))) {
            const HRESULT shown = dialog->Show(nullptr);
            if (test == NativeCase::ExplicitFallback || test == NativeCase::StopActive ||
                test == NativeCase::SwitchToWindows || test == NativeCase::TerminateWatcher) {
                result = host_called && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : 1;
            } else if (host_called && shown == S_OK) {
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

bool WatcherCase(const std::wstring& folder, NativeCase test) {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
    const wchar_t* argument = test == NativeCase::ExplicitFallback ? L"--native-fallback" :
        test == NativeCase::CustomConfirm ? L"--native-custom-confirm" :
        test == NativeCase::StopActive ? L"--native-stop" :
        test == NativeCase::SwitchToWindows ? L"--native-switch" :
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
                return IsWindowVisible(native) && IsWindowEnabled(native) &&
                    GetWindowLongPtrW(native, GWL_EXSTYLE) == native_style &&
                    !(GetWindowLongPtrW(picker, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
                    CoversSource(picker, native) && AboveSource(picker, native);
            }, 2000);
            if (!success) std::puts("[DIAG] picker did not cover the unchanged native dialog");
        }
        if (success && (external_watcher || test == NativeCase::StopActive || test == NativeCase::SwitchToWindows)) {
            if (test == NativeCase::StopActive) {
                watcher.Stop();
                success = watcher.StoppedForTesting();
            } else if (external_watcher) {
                success = TerminateProcess(watcher_process.Get(), 2) &&
                    WaitForSingleObject(watcher_process.Get(), 2000) == WAIT_OBJECT_0;
            } else {
                RECT client{};
                GetClientRect(picker, &client);
                const int dpi = pulse::compat::WindowDpi(picker);
                const auto button = MAKELPARAM(MulDiv(45, dpi, 96), client.bottom - MulDiv(30, dpi, 96));
                success = PostMessageW(picker, WM_LBUTTONDOWN, MK_LBUTTON, button) &&
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
            HWND edit = nullptr;
            const auto path = folder + L"\\sample.txt";
            DWORD_PTR changed = FALSE;
            success = success && WaitFor([&] {
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
    if (watcher_process && WaitForSingleObject(watcher_process.Get(), 2000) == WAIT_TIMEOUT)
        TerminateProcess(watcher_process.Get(), 2);
    watcher.Stop();
    const bool stopped = watcher.StoppedForTesting();
    if (!stopped) std::puts("[DIAG] foreground observer did not stop");
    return success && status == 0 && stopped;
}

bool BridgeCase(const std::wstring& directory, const std::filesystem::path& pulse, wire::Operation operation,
                bool no_test = false, bool cancel_open = false, const wchar_t* open_name = L"sample.txt",
                bool preserve_link = false) {
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
    std::atomic_bool finished{false};
    bool saw_picker = false, saw_filename = false;
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
                    if (IsWindowEnabled(edit)) PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 1);
                }
                return false;
            }, 12000)) SetEvent(cancel.Get());
    });
    const auto response = wire::RunPickerClient(pulse.wstring(), message, nullptr, cancel.Get());
    finished = true;
    input.join();
    const auto* result = std::get_if<wire::Response>(&response.body);
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

bool SidebarCase(const std::filesystem::path& folder) {
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

    ui::FilePickerSpec spec;
    spec.title = L"Pulse fixture - picker sidebar";
    spec.software_device = true;
    spec.load_places = [=](const auto&) {
        return ui::PickerPlaces{{{L"OneDrive account", cloud, L"\xE753"}},
            {{L"First pin", first, L"\xE8B7"}, {L"Second pin", second, L"\xE8B7"}},
            {{L"Saved share", network, L"\xE968"}}};
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
                if (FindFilename(window, first.c_str())) return true;
                click(157); // 100 DIP list origin, 30 DIP account and 12 DIP divider.
                return false;
            }, 3500);
            if (navigated) {
                key(VK_HOME);
                key(VK_RETURN);
                navigated = WaitFor([&] { return FindFilename(window, cloud.c_str()); }, 2000);
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
    if (FAILED(OleInitialize(nullptr))) return false;
    ui::FilePickerResult selection;
    const bool picked = ui::ShowFilePicker(nullptr, spec, true, {0.20f, 0.50f, 0.85f, 1.0f}, selection);
    if (driver.joinable()) driver.join();
    OleUninitialize();
    return navigated && !picked;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3) {
        const std::wstring_view argument = argv[1];
        if (argument == L"--native") return NativeFixture(argv[2], NativeCase::Open);
        if (argument == L"--native-fallback") return NativeFixture(argv[2], NativeCase::ExplicitFallback);
        if (argument == L"--native-custom-confirm") return NativeFixture(argv[2], NativeCase::CustomConfirm);
        if (argument == L"--native-stop") return NativeFixture(argv[2], NativeCase::StopActive);
        if (argument == L"--native-switch") return NativeFixture(argv[2], NativeCase::SwitchToWindows);
        if (argument == L"--native-terminate-watcher") return NativeFixture(argv[2], NativeCase::TerminateWatcher);
        if (argument == L"--watch-native") {
            wchar_t* end = nullptr;
            const DWORD source_process = std::wcstoul(argv[2], &end, 10);
            return source_process && !*end ? WatchNativeFixture(source_process) : 2;
        }
    }
    const bool watcher_only = argc == 3 && wcscmp(argv[2], L"--watcher-only") == 0;
    const bool stop_only = argc == 3 && wcscmp(argv[2], L"--watcher-stop-only") == 0;
    const bool switch_only = argc == 3 && wcscmp(argv[2], L"--watcher-switch-only") == 0;
    const bool terminate_only = argc == 3 && wcscmp(argv[2], L"--watcher-termination-only") == 0;
    const bool fallback_only = argc == 3 && wcscmp(argv[2], L"--fallback-only") == 0;
    const bool custom_open_only = argc == 3 && wcscmp(argv[2], L"--custom-open-only") == 0;
    const bool host_only = argc == 3 && wcscmp(argv[2], L"--host-only") == 0;
    const bool sidebar_only = argc == 3 && wcscmp(argv[2], L"--sidebar-only") == 0;
    if (argc != 2 && !watcher_only && !stop_only && !switch_only && !terminate_only && !fallback_only &&
        !custom_open_only && !host_only && !sidebar_only) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto folder = std::filesystem::absolute(argv[1]) /
        (L"picker-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directories(folder)) return 2;
    std::ofstream(folder / L"sample.txt") << "fixture";
    if (sidebar_only) {
        const bool sidebar = SidebarCase(folder);
        std::printf("[%s] Picker sidebar loads saved places and navigates all three groups\n", sidebar ? "PASS" : "FAIL");
        return sidebar ? 0 : 1;
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
        std::printf("[%s] Window watcher submits after the Shell bridge falls back automatically\n",
            taken_over ? "PASS" : "FAIL");
        ok &= taken_over;
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
