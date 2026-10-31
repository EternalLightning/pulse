// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "shell_query_worker.h"
#include "shell_query_icons.h"
#include "shell_query_metadata.h"
#include "../common/current_user_security.h"
#include <objbase.h>

namespace pulse::shell {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, ended{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &ended, &kernel, &user)
        ? (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
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
    const HANDLE waits[]{event.value, parent};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 8000);
    if (wait != WAIT_OBJECT_0) CancelIoEx(pipe, &operation);
    DWORD ignored = 0;
    const bool connected = GetOverlappedResult(pipe, &operation, &ignored, TRUE) != FALSE;
    return wait == WAIT_OBJECT_0 && connected;
}

ipc::ShellQueryResult Query(const ipc::ShellQueryRequest& request) {
    ipc::ShellQueryResult result;
#ifdef PULSE_SHELL_QUERY_TESTING
    if (request.path.starts_with(L":pulse-shell-query-hang:")) {
        const auto event_name = request.path.substr(wcslen(L":pulse-shell-query-hang:"));
        Handle ready;
        ready.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str());
        if (ready.value) SetEvent(ready.value);
        Sleep(INFINITE);
    }
    if (request.path == L":pulse-shell-query-crash:") TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
    if (request.path == L":pulse-shell-query-invalid-pidl:") {
        result.error = S_OK;
        result.pidl = {3, 0, 9};
        return result;
    }
    if (request.kind == ipc::ShellQueryKind::InitializePreviewItem &&
        request.path.starts_with(L":pulse-shell-preview-lost:")) {
        auto initialization = request;
        initialization.path.erase(0, wcslen(L":pulse-shell-preview-lost:"));
        InitializePreviewItem(initialization, result);
        TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
        return result;
    }
    if (request.path == L":pulse-shell-query-ready:" || request.path == L":pulse-shell-query-fragment:") {
        result.error = S_OK;
        result.text = std::to_wstring(GetCurrentProcessId());
        return result;
    }
#endif
    switch (request.kind) {
    case ipc::ShellQueryKind::Icon: QueryShellIcons(request, result); break;
    case ipc::ShellQueryKind::TypeName:
    case ipc::ShellQueryKind::DisplayName: QueryShellName(request, result); break;
    case ipc::ShellQueryKind::Shortcut: QueryShellShortcut(request, result); break;
    case ipc::ShellQueryKind::ParsePidl:
    case ipc::ShellQueryKind::PidlName:
    case ipc::ShellQueryKind::FolderIdentity: QueryShellNamespace(request, result); break;
    case ipc::ShellQueryKind::InitializePreviewItem: InitializePreviewItem(request, result); break;
    }
    return result;
}
}

int RunShellQueryWorker(DWORD parent_pid, uint32_t session) {
    Handle parent;
    parent.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
    const auto created = parent.value ? CreationTime(parent.value) : 0;
    if (!created || created > CreationTime(GetCurrentProcess()) ||
        WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) return 1;
    pulse::CurrentUserSecurityAttributes security;
    if (!security) return 2;
    Handle pipe;
    const auto name = ipc::ShellQueryPipeName(parent_pid, session);
    pipe.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, security.get());
    if (pipe.value == INVALID_HANDLE_VALUE || !Connect(pipe.value, parent.value)) return 2;
    ULONG peer = 0;
    if (!GetNamedPipeClientProcessId(pipe.value, &peer) || peer != parent_pid) return 3;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 4;
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);
    ipc::PipeIoOptions options;
    options.pending_timeout_ms = 60000;
    options.cancel_event = parent.value;
    for (;;) {
        ipc::MsgHeader header;
        if (!ipc::PipeRead(pipe.value, &header, sizeof(header), options) || header.magic != ipc::kMagic ||
            header.type != ipc::kShellQueryRequestFrame || !header.request_id ||
            header.payload_size > ipc::kMaxPayload) break;
        std::vector<uint8_t> bytes(header.payload_size);
        if (!ipc::PipeRead(pipe.value, bytes.data(), header.payload_size, options)) break;
        ipc::ShellQueryRequest request;
        ipc::ShellQueryResult result;
        try {
            if (!ipc::ReadShellQuery(bytes.data(), bytes.size(), request)) result.error = E_INVALIDARG;
            else result = Query(request);
        } catch (const std::bad_alloc&) { result = {}; result.error = E_OUTOFMEMORY; }
        catch (...) { result = {}; result.error = E_FAIL; }
        bytes = ipc::WriteShellQueryResult(result);
        header.type = ipc::kShellQueryResponseFrame;
        header.payload_size = static_cast<uint32_t>(bytes.size());
#ifdef PULSE_SHELL_QUERY_TESTING
        if (request.path == L":pulse-shell-query-fragment:") {
            if (!ipc::PipeWrite(pipe.value, &header, sizeof(header), options)) break;
            for (auto& byte : bytes) {
                if (!ipc::PipeWrite(pipe.value, &byte, 1, options)) return 5;
                Sleep(75);
            }
            continue;
        }
#endif
        if (!ipc::PipeWrite(pipe.value, &header, sizeof(header), options) ||
            !ipc::PipeWrite(pipe.value, bytes.data(), header.payload_size, options)) break;
    }
    return 0;
}

} // namespace pulse::shell
