#include "shell_launch_process.h"
#include "shell_interaction_worker.h"
#include "ctx_session_process.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/command_line.h"
#include "../ops/image_open.h"
#ifdef PULSE_SHELL_LAUNCH_TESTING
#include "../common/utf8_file.h"
#endif
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <iterator>
#include <memory>
#include <algorithm>

namespace pulse::shell {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct PidlFree {
    using pointer = LPITEMIDLIST;
    void operator()(pointer pidl) const { CoTaskMemFree(pidl); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, PidlFree>;
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool Send(HANDLE pipe, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    ipc::MsgHeader header;
    header.type = type;
    header.request_id = id;
    header.payload_size = static_cast<uint32_t>(payload.size());
    ipc::PipeIoOptions options;
    options.pending_timeout_ms = 1000;
    return ipc::PipeWrite(pipe, &header, sizeof(header), options) &&
        ipc::PipeWrite(pipe, payload.data(), header.payload_size, options);
}

bool Connect(HANDLE pipe, HANDLE parent) {
    Handle event;
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    const HANDLE handles[]{event.value, parent};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 8000);
    if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &operation);
    DWORD ignored = 0;
    const bool connected = GetOverlappedResult(pipe, &operation, &ignored, TRUE) != FALSE;
    return wait == WAIT_OBJECT_0 && connected;
}

HWND ValidateOwner(const ipc::ShellLaunchRequest& request, DWORD ui_pid, uint64_t created, Handle& ui) {
    if (!request.owner_hwnd || !ui_pid || !created) return nullptr;
    ui.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ui_pid);
    if (!ui.value || CreationTime(ui.value) != created ||
        WaitForSingleObject(ui.value, 0) != WAIT_TIMEOUT) return nullptr;
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.owner_hwnd));
    DWORD process = 0;
    return IsWindow(owner) && GetWindowThreadProcessId(owner, &process) && process == ui_pid ? owner : nullptr;
}

HRESULT ShellError() {
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

std::wstring ErrorDetail(HRESULT error, const ipc::ShellLaunchRequest& request) {
    wchar_t code[32]{};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(error));
    std::wstring detail = L"Shell launch failed (" + std::wstring(code) + L")";
    if (!request.paths.empty()) detail += L" for " + request.paths.front();
    wchar_t* message = nullptr;
    const DWORD system_error = HRESULT_FACILITY(error) == FACILITY_WIN32 ? HRESULT_CODE(error) :
        static_cast<DWORD>(error);
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, system_error, 0,
            reinterpret_cast<wchar_t*>(&message), 0, nullptr) && message) {
        std::wstring text(message);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
        detail += L": " + text;
    }
    if (message) LocalFree(message);
    return detail;
}

HRESULT MultiProperties(const std::vector<std::wstring>& paths, ComPtr<IDataObject>& data) {
    std::vector<Pidl> owned;
    std::vector<PCIDLIST_ABSOLUTE> pidls;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE raw = nullptr;
        const auto shell_path = pulse::path::StripExtendedPathPrefix(path);
        const HRESULT result = SHParseDisplayName(shell_path.c_str(), nullptr, &raw, 0, nullptr);
        owned.emplace_back(raw);
        if (FAILED(result)) return result;
        if (!raw) return E_FAIL;
        pidls.push_back(raw);
    }
    ComPtr<IShellItemArray> array;
    HRESULT result = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), pidls.data(), &array);
    if (FAILED(result)) return result;
    result = array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    return FAILED(result) ? result : SHMultiFileProperties(data.Get(), 0);
}

std::wstring OpenWorkingDirectory(const std::wstring& path) {
    const auto plain = pulse::path::StripExtendedPathPrefix(path);
    if (plain.empty()) return {};
    const auto colon = plain.find(L':');
    if (colon != std::wstring::npos && colon != 1) return {};
    if (plain.starts_with(L"\\\\")) {
        const auto share = plain.find(L'\\', 2);
        if (share == std::wstring::npos || share + 1 == plain.size()) return {};
    }
    const DWORD capacity = GetFullPathNameW(plain.c_str(), 0, nullptr, nullptr);
    if (!capacity) return {};
    std::wstring full(capacity, L'\0');
    const DWORD length = GetFullPathNameW(plain.c_str(), capacity, full.data(), nullptr);
    if (!length || length >= capacity) return {};
    full.resize(length);
    if (PathIsRootW(full.c_str()) || plain.back() == L'\\' || plain.back() == L'/') return full;
    const auto separator = full.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    return full.substr(0, separator == 2 && full[1] == L':' ? 3 : separator);
}

bool SplitLaunchCommand(const std::wstring& command, std::wstring& executable,
                         std::wstring& parameters) {
    const auto begin = command.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return false;
    size_t end = begin;
    if (command[begin] == L'"') {
        end = command.find(L'"', begin + 1);
        if (end == std::wstring::npos || end == begin + 1 ||
            (end + 1 < command.size() && command[end + 1] != L' ' && command[end + 1] != L'\t'))
            return false;
        executable = command.substr(begin + 1, end - begin - 1);
        ++end;
    } else {
        end = command.find_first_of(L" \t", begin);
        executable = command.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (executable.find(L'"') != std::wstring::npos) return false;
        const bool path = executable.find_first_of(L"\\/:") != std::wstring::npos;
        if (path) {
            // Never probe whitespace prefixes of an unquoted registry path.
            // Such paths must name their executable extension in this token.
            const auto extension = executable.find_last_of(L'.');
            if (extension == std::wstring::npos ||
                    (_wcsicmp(executable.c_str() + extension, L".exe") != 0 &&
                     _wcsicmp(executable.c_str() + extension, L".com") != 0)) return false;
        } else {
            const DWORD capacity = SearchPathW(nullptr, executable.c_str(), L".exe", 0, nullptr, nullptr);
            if (!capacity) return false;
            std::wstring resolved(capacity, L'\0');
            const DWORD length = SearchPathW(nullptr, executable.c_str(), L".exe", capacity, resolved.data(), nullptr);
            if (!length || length >= capacity) return false;
            resolved.resize(length);
            executable = std::move(resolved);
        }
    }
    const auto arguments = end == std::wstring::npos ? end : command.find_first_not_of(L" \t", end);
    parameters = arguments == std::wstring::npos ? L"" : command.substr(arguments);
    return true;
}

HRESULT Execute(const ipc::ShellLaunchRequest& request, HWND owner, ComPtr<IDataObject>& data,
                std::vector<std::unique_ptr<ops::ImageOpenSession>>& image_sessions,
                std::wstring& detail, const std::function<bool()>& heartbeat) {
    if (request.kind == ipc::ShellLaunchKind::PickFile || request.kind == ipc::ShellLaunchKind::PickFolder)
        return PickShellPath(request, owner, detail);
    if (request.kind == ipc::ShellLaunchKind::ElevatedWait) return RunElevated(request, detail, heartbeat);
    if (request.kind == ipc::ShellLaunchKind::Command) {
        // Registry verbs carry a complete expanded command line, including its own quoting.
        std::wstring command = request.executable;
        if (!request.parameters.empty()) command += L" " + request.parameters;
        std::wstring executable, parameters;
        if (!SplitLaunchCommand(command, executable, parameters)) return E_INVALIDARG;
        auto process_command = QuoteWindowsArgument(executable);
        if (!parameters.empty()) process_command += L" " + parameters;
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
        startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable.c_str(), process_command.data(), nullptr, nullptr, FALSE,
                0, nullptr, request.directory.empty() ? nullptr : request.directory.c_str(), &startup, &child)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ELEVATION_REQUIRED) return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
            SHELLEXECUTEINFOW elevated{sizeof(elevated)};
            elevated.hwnd = owner;
            elevated.lpVerb = L"runas";
            elevated.lpFile = executable.c_str();
            elevated.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
            elevated.lpDirectory = request.directory.empty() ? nullptr : request.directory.c_str();
            elevated.nShow = SW_SHOWNORMAL;
            elevated.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            return ShellExecuteExW(&elevated) ? S_OK : ShellError();
        }
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return S_OK;
    }
    if (request.kind == ipc::ShellLaunchKind::Reveal) {
        PIDLIST_ABSOLUTE raw = nullptr;
        const auto path = pulse::path::StripExtendedPathPrefix(request.paths.front());
        HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr);
        Pidl target(raw);
        if (FAILED(result)) return result;
        Pidl folder(ILCloneFull(target.get()));
        if (!folder) return E_OUTOFMEMORY;
        if (!ILRemoveLastID(folder.get())) return SHOpenFolderAndSelectItems(target.get(), 0, nullptr, 0);
        PCUITEMID_CHILD child = ILFindLastID(target.get());
        return SHOpenFolderAndSelectItems(folder.get(), 1, &child, 0);
    }
    const bool properties = _wcsicmp(request.verb.c_str(), L"properties") == 0;
    if (properties && request.paths.size() > 1) return MultiProperties(request.paths, data);
    const auto paths = request.executable.empty() ? request.paths : std::vector<std::wstring>{request.executable};
    for (const auto& original : paths) {
        const auto path = pulse::path::StripExtendedPathPrefix(original);
        const auto directory = request.directory.empty() && request.executable.empty()
            ? OpenWorkingDirectory(path) : request.directory;
        if (_wcsicmp(request.verb.c_str(), L"openas") == 0 || _wcsicmp(request.verb.c_str(), L"openwith") == 0) {
            SHELLEXECUTEINFOW chooser{sizeof(chooser)};
            chooser.hwnd = owner;
            chooser.lpVerb = L"openas";
            chooser.lpFile = path.c_str();
            chooser.nShow = SW_SHOWNORMAL;
            // Resolve the system chooser independently of the file's current association.
            chooser.fMask = SEE_MASK_CLASSNAME | SEE_MASK_NOASYNC;
            chooser.lpClass = L"Unknown";
            if (!ShellExecuteExW(&chooser)) return ShellError();
            continue;
        }
        if (properties && SHObjectProperties(owner, SHOP_FILEPATH, path.c_str(), nullptr)) continue;
        if (!properties && request.executable.empty() && request.parameters.empty() &&
            (request.verb.empty() || _wcsicmp(request.verb.c_str(), L"open") == 0) &&
            (request.flags & ipc::kShellLaunchImageNeighbors)) {
            auto session = std::make_unique<ops::ImageOpenSession>();
            image_sessions.push_back(std::move(session));
            if (ops::TryOpenImageWithNeighbors(owner, original, {}, image_sessions.back().get())) continue;
            image_sessions.pop_back();
        }
        SHELLEXECUTEINFOW shell{sizeof(shell)};
        shell.hwnd = owner;
        shell.lpVerb = properties ? L"properties"
            : request.verb.empty() ? L"open" : request.verb.c_str();
        shell.lpFile = path.c_str();
        shell.lpParameters = request.parameters.empty() ? nullptr : request.parameters.c_str();
        shell.lpDirectory = directory.empty() ? nullptr : directory.c_str();
        shell.nShow = SW_SHOWNORMAL;
        shell.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | (properties ? SEE_MASK_INVOKEIDLIST : 0);
        if (!ShellExecuteExW(&shell)) return ShellError();
    }
    return S_OK;
}

#ifdef PULSE_SHELL_LAUNCH_TESTING
LRESULT CALLBACK FixtureWindow(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool Fixture(const ipc::ShellLaunchRequest& request, HRESULT& result, std::wstring& detail) {
    if (request.paths.empty() || !request.paths.front().starts_with(L":pulse-shell-launch-")) return false;
    if (!request.directory.empty()) {
        Handle file;
        file.value = CreateFileW(request.directory.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value != INVALID_HANDLE_VALUE) {
            const auto pid = std::to_string(GetCurrentProcessId());
            DWORD written = 0;
            WriteFile(file.value, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
        }
    }
    if (!request.parameters.empty()) {
        Handle event;
        event.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, request.parameters.c_str());
        if (event.value) SetEvent(event.value);
    }
    const auto& hook = request.paths.front();
    if (hook == L":pulse-shell-launch-hang:") Sleep(INFINITE);
    if (hook == L":pulse-shell-launch-crash:") TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
    detail = L"worker PID " + std::to_wstring(GetCurrentProcessId());
    result = S_OK;
    if (hook == L":pulse-shell-launch-modeless:") {
        WNDCLASSW window_class{};
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpfnWndProc = FixtureWindow;
        window_class.lpszClassName = L"PulseShellLaunchFixture";
        RegisterClassW(&window_class);
        HWND window = CreateWindowW(window_class.lpszClassName, L"Pulse shell launch fixture",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300, 180,
            nullptr, nullptr, window_class.hInstance, nullptr);
        if (!window) result = ShellError();
    } else if (hook == L":pulse-shell-launch-child:") {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --launch-test-child";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &child)) {
            detail += L"; child PID " + std::to_wstring(child.dwProcessId);
            if (!request.directory.empty()) {
                Handle file;
                const auto name = request.directory + L".child";
                file.value = CreateFileW(name.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file.value != INVALID_HANDLE_VALUE) {
                    const auto pid = std::to_string(child.dwProcessId);
                    DWORD written = 0;
                    WriteFile(file.value, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
                }
            }
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        } else result = ShellError();
    } else if (hook != L":pulse-shell-launch-ready:") result = E_INVALIDARG;
    return true;
}
#endif
}

#ifdef PULSE_SHELL_LAUNCH_TESTING
int RunShellLaunchTestCommand(const wchar_t* output) {
    const DWORD capacity = GetCurrentDirectoryW(0, nullptr);
    if (!capacity) return 1;
    std::vector<wchar_t> buffer(capacity);
    const DWORD length = GetCurrentDirectoryW(capacity, buffer.data());
    if (!length || length >= capacity) return 1;
    const auto text = std::to_wstring(GetCurrentProcessId()) + L"\n" +
        std::wstring(buffer.data(), length) + L"\n";
    return pulse::WriteUtf8FileAtomic(output, text) ? 0 : 2;
}
#endif

int RunShellLaunchWorker(DWORD parent_pid, uint32_t request_id) {
    Handle parent;
    parent.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
    if (!parent.value || !CreationTime(parent.value) ||
        CreationTime(parent.value) > CreationTime(GetCurrentProcess())) return 1;
    pulse::CurrentUserSecurityAttributes security;
    if (!security) return 2;
    Handle pipe;
    const auto name = ShellLaunchPipeName(parent_pid, request_id);
    pipe.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, security.get());
    if (pipe.value == INVALID_HANDLE_VALUE || !Connect(pipe.value, parent.value)) return 2;
    ULONG peer = 0;
    if (!GetNamedPipeClientProcessId(pipe.value, &peer) || peer != parent_pid) return 3;
    ipc::PipeIoOptions options;
    options.pending_timeout_ms = 8000;
    ipc::MsgHeader header{};
    if (!ipc::PipeRead(pipe.value, &header, sizeof(header), options) || header.magic != ipc::kMagic ||
        header.type != ipc::REQ_SHELL_LAUNCH || header.request_id != request_id ||
        header.payload_size > ipc::kMaxPayload || header.payload_size < 12) return 3;
    std::vector<uint8_t> payload(header.payload_size);
    if (!ipc::PipeRead(pipe.value, payload.data(), header.payload_size, options)) return 3;
    ipc::PayloadReader identity(payload.data(), 12);
    uint32_t ui_pid = 0;
    uint64_t ui_created = 0;
    if (!identity.GetU32(ui_pid) || !identity.GetU64(ui_created)) return 3;
    ipc::ShellLaunchRequest request;
    if (!ipc::ReadShellLaunch(payload.data() + 12, payload.size() - 12, request)) return 3;
    Apartment apartment;
    if (FAILED(apartment.result)) return 4;
    Handle ui;
    const HWND owner = ValidateOwner(request, ui_pid, ui_created, ui);
    ComPtr<IDataObject> data;
    std::vector<std::unique_ptr<ops::ImageOpenSession>> image_sessions;
    HRESULT result = E_FAIL;
    std::wstring detail;
    try {
#ifdef PULSE_SHELL_LAUNCH_TESTING
        if (!Fixture(request, result, detail))
#endif
            result = request.owner_hwnd && !owner &&
                (request.kind == ipc::ShellLaunchKind::PickFile || request.kind == ipc::ShellLaunchKind::PickFolder)
                ? E_ACCESSDENIED : Execute(request, owner, data, image_sessions, detail, [&] {
                return WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT &&
                    Send(pipe.value, kShellLaunchHeartbeat, request_id, {});
            });
    } catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; }
    catch (...) { result = E_FAIL; }
#ifdef PULSE_SHELL_LAUNCH_TESTING
    if (detail.empty() && SUCCEEDED(result)) detail = L"worker PID " + std::to_wstring(GetCurrentProcessId());
#endif
    const bool cancelled = result == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    if (FAILED(result) && detail.empty()) detail = cancelled ? L"Shell launch cancelled." : ErrorDetail(result, request);
    ipc::PayloadWriter response;
    response.PutU32(static_cast<uint32_t>(result));
    response.PutU32(cancelled ? 1u : 0u);
    response.PutString(detail);
    if (!Send(pipe.value, ipc::RSP_SHELL_LAUNCH_DONE, request_id, response.data())) return 5;
    if (request.kind == ipc::ShellLaunchKind::PickFile || request.kind == ipc::ShellLaunchKind::PickFolder ||
        request.kind == ipc::ShellLaunchKind::ElevatedWait) return 0;

    const auto started = GetTickCount64();
    auto heartbeat = started;
    for (;;) {
        if (WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) break;
        MsgWaitForMultipleObjectsEx(1, &parent.value, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (!IsDialogMessageW(GetActiveWindow(), &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        const auto now = GetTickCount64();
        if (now - heartbeat >= 500) {
            if (!Send(pipe.value, kShellLaunchHeartbeat, request_id, {})) break;
            heartbeat = now;
        }
        std::erase_if(image_sessions, [](const auto& session) { return !session->ViewerRunning(); });
        ULONG references = 1;
        if (data) { data->AddRef(); references = data->Release(); }
        if (now - started > 3000 && !ContextProcessHasVisibleWindows(GetCurrentProcessId()) &&
            references <= 1 && image_sessions.empty()) break;
    }
    data.Reset();
    return 0;
}
} // namespace pulse::shell
