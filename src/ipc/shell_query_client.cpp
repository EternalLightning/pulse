#include "shell_query_client.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <chrono>

namespace pulse::ipc {
namespace {
constexpr size_t kQueryProcessLimit = 4;

struct QuerySlot {
    HANDLE process = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD pid = 0;
    bool leased = false;
    bool poisoned = false;
};

struct QueryPool {
    std::mutex mutex;
    std::condition_variable available;
    std::array<QuerySlot, kQueryProcessLimit> slots;
    HANDLE job = nullptr;
    std::atomic<uint32_t> next_session{static_cast<uint32_t>(GetTickCount64())};
    std::atomic<uint32_t> next_request{1};
    std::wstring executable;

    QueryPool() {
        wchar_t module[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
        if (length && length < ARRAYSIZE(module)) {
            executable.assign(module, length);
            const auto separator = executable.find_last_of(L"\\/");
            if (separator != std::wstring::npos) executable.resize(separator + 1);
            else executable.clear();
            executable += L"Pulse.Shell.exe";
        }
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job); job = nullptr;
        }
    }

    void ReapLocked() {
        for (auto& slot : slots) {
            if (!slot.leased && slot.poisoned && slot.process &&
                WaitForSingleObject(slot.process, 0) == WAIT_OBJECT_0) {
                CloseHandle(slot.process); slot.process = nullptr;
                slot.pid = 0; slot.poisoned = false;
            }
        }
    }
};

QueryPool& Pool() {
    // Background consumers can outlive window teardown. Kernel job ownership
    // reclaims all four helpers when the UI process exits, without a UI join.
    static auto* pool = new QueryPool;
    return *pool;
}

struct Lease {
    QueryPool& pool;
    QuerySlot* slot = nullptr;
    explicit Lease(QueryPool& owner) : pool(owner) {}
    ~Lease() {
        if (slot) {
            std::lock_guard lock(pool.mutex);
            slot->leased = false;
            pool.available.notify_one();
        }
    }
};

bool Cancelled(const std::function<bool()>& cancelled, ULONGLONG deadline) noexcept {
    try {
        if (cancelled && cancelled()) { SetLastError(ERROR_CANCELLED); return true; }
    } catch (...) { SetLastError(ERROR_CANCELLED); return true; }
    if (GetTickCount64() >= deadline) { SetLastError(ERROR_TIMEOUT); return true; }
    return false;
}

bool Acquire(Lease& lease, const std::function<bool()>& cancelled, ULONGLONG deadline) {
    for (;;) {
        if (Cancelled(cancelled, deadline)) return false;
        std::unique_lock lock(lease.pool.mutex);
        lease.pool.ReapLocked();
        for (auto& slot : lease.pool.slots) {
            if (!slot.leased && !slot.poisoned) {
                slot.leased = true; lease.slot = &slot;
                return true;
            }
        }
        lease.pool.available.wait_for(lock, std::chrono::milliseconds(20));
    }
}

void Retire(QuerySlot& slot) noexcept {
    if (slot.pipe != INVALID_HANDLE_VALUE) { CloseHandle(slot.pipe); slot.pipe = INVALID_HANDLE_VALUE; }
    if (!slot.process) return;
    if (WaitForSingleObject(slot.process, 0) == WAIT_TIMEOUT) TerminateProcess(slot.process, ERROR_CANCELLED);
    // A slot cannot be reused while a stuck provider's process still exists.
    if (WaitForSingleObject(slot.process, 1000) == WAIT_OBJECT_0) {
        CloseHandle(slot.process); slot.process = nullptr; slot.pid = 0;
    } else slot.poisoned = true;
}

bool Connect(Lease& lease, const std::function<bool()>& cancelled, ULONGLONG deadline) {
    auto& slot = *lease.slot;
    if (slot.process && slot.pipe != INVALID_HANDLE_VALUE &&
        WaitForSingleObject(slot.process, 0) == WAIT_TIMEOUT) return true;
    Retire(slot);
    if (slot.poisoned) { SetLastError(ERROR_NOT_READY); return false; }
    if (!lease.pool.job || lease.pool.executable.empty()) { SetLastError(ERROR_NOT_READY); return false; }
    uint32_t session = lease.pool.next_session.fetch_add(1);
    if (!session) session = lease.pool.next_session.fetch_add(1);
    const auto name = ShellQueryPipeName(GetCurrentProcessId(), session);
    std::wstring command = L"\"" + lease.pool.executable + L"\" --query-worker " +
        std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(session);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(lease.pool.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return false;
    slot.process = child.hProcess; slot.pid = child.dwProcessId;
    const bool assigned = AssignProcessToJobObject(lease.pool.job, child.hProcess) != FALSE;
    const bool resumed = assigned && ResumeThread(child.hThread) != static_cast<DWORD>(-1);
    CloseHandle(child.hThread);
    if (!resumed) { Retire(slot); SetLastError(ERROR_NOT_READY); return false; }
    while (!Cancelled(cancelled, deadline)) {
        if (WaitForSingleObject(slot.process, 0) != WAIT_TIMEOUT) {
            SetLastError(ERROR_PROCESS_ABORTED); return false;
        }
        slot.pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (slot.pipe != INVALID_HANDLE_VALUE) {
            ULONG pid = 0;
            if (GetNamedPipeServerProcessId(slot.pipe, &pid) && pid == slot.pid) return true;
            SetLastError(ERROR_ACCESS_DENIED); return false;
        }
        Sleep(10);
    }
    return false;
}

template<bool Writing>
bool Transfer(HANDLE pipe, void* data, size_t size, const std::function<bool()>& cancelled,
              ULONGLONG deadline) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{event};
    auto* cursor = static_cast<uint8_t*>(data);
    while (size) {
        if (Cancelled(cancelled, deadline)) return false;
        OVERLAPPED operation{};
        operation.hEvent = event;
        ResetEvent(event);
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 64 * 1024));
        DWORD done = 0;
        BOOL ok;
        if constexpr (Writing) ok = WriteFile(pipe, cursor, chunk, &done, &operation);
        else ok = ReadFile(pipe, cursor, chunk, &done, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            for (;;) {
                const DWORD wait = WaitForSingleObject(event, 20);
                if (wait == WAIT_OBJECT_0) { ok = GetOverlappedResult(pipe, &operation, &done, FALSE); break; }
                if (wait == WAIT_FAILED || Cancelled(cancelled, deadline)) {
                    const DWORD error = GetLastError();
                    CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &done, TRUE);
                    SetLastError(error);
                    return false;
                }
            }
        }
        if (!ok) return false;
        if (!done) { SetLastError(ERROR_BROKEN_PIPE); return false; }
        cursor += done; size -= done;
    }
    return true;
}

bool Exchange(Lease& lease, const ShellQueryRequest& request, ShellQueryResult& result,
              const std::function<bool()>& cancelled, ULONGLONG deadline) {
    auto bytes = WriteShellQuery(request);
    MsgHeader header;
    header.type = kShellQueryRequestFrame;
    header.request_id = lease.pool.next_request.fetch_add(1);
    if (!header.request_id) header.request_id = lease.pool.next_request.fetch_add(1);
    header.payload_size = static_cast<uint32_t>(bytes.size());
    const uint32_t id = header.request_id;
    if (!Transfer<true>(lease.slot->pipe, &header, sizeof(header), cancelled, deadline) ||
        !Transfer<true>(lease.slot->pipe, bytes.data(), bytes.size(), cancelled, deadline) ||
        !Transfer<false>(lease.slot->pipe, &header, sizeof(header), cancelled, deadline)) return false;
    if (header.magic != kMagic || header.type != kShellQueryResponseFrame || header.request_id != id ||
        header.payload_size > kMaxPayload) { SetLastError(ERROR_INVALID_DATA); return false; }
    bytes.resize(header.payload_size);
    if (!Transfer<false>(lease.slot->pipe, bytes.data(), bytes.size(), cancelled, deadline)) return false;
    ShellQueryResult decoded;
    const bool pidl_result = request.kind == ShellQueryKind::ParsePidl ||
        request.kind == ShellQueryKind::FolderIdentity;
    const bool preview_item = request.kind == ShellQueryKind::InitializePreviewItem;
    if (!ReadShellQueryResult(bytes.data(), bytes.size(), decoded) ||
        (request.kind == ShellQueryKind::Icon && SUCCEEDED(decoded.error) && decoded.icons.empty()) ||
        (request.kind != ShellQueryKind::Icon && !decoded.icons.empty()) ||
        (!pidl_result && !decoded.pidl.empty()) ||
        (request.kind != ShellQueryKind::FolderIdentity && !decoded.filesystem_path.empty()) ||
        (SUCCEEDED(decoded.error) && pidl_result && !ValidShellPidl(decoded.pidl)) ||
        (preview_item && (!decoded.text.empty() || decoded.target_is_dir || decoded.target_size ||
            decoded.target_mtime.dwLowDateTime || decoded.target_mtime.dwHighDateTime)) ||
        (SUCCEEDED(decoded.error) && !pidl_result && !preview_item && request.kind != ShellQueryKind::Icon && decoded.text.empty())) {
        SetLastError(ERROR_INVALID_DATA); return false;
    }
    if (Cancelled(cancelled, deadline)) return false;
    result = std::move(decoded);
    return true;
}
}

bool QueryShell(const ShellQueryRequest& request, ShellQueryResult& result,
                const std::function<bool()>& cancelled, DWORD timeout_ms) {
    result = {};
    if (!timeout_ms || timeout_ms > 60000) { result.error = E_INVALIDARG; return false; }
    ShellQueryRequest validated;
    const auto bytes = WriteShellQuery(request);
    if (!ReadShellQuery(bytes.data(), bytes.size(), validated)) { result.error = E_INVALIDARG; return false; }
    Lease lease(Pool());
    const auto deadline = GetTickCount64() + timeout_ms;
    DWORD error = ERROR_GEN_FAILURE;
    try {
        if (!Acquire(lease, cancelled, deadline)) {
            result.error = HRESULT_FROM_WIN32(GetLastError()); return false;
        }
        // Initializing a preview handler mutates its state. A lost response
        // must never replay the initialization against the same COM object.
        const int attempts = request.kind == ShellQueryKind::InitializePreviewItem ? 1 : 2;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (Connect(lease, cancelled, deadline) && Exchange(lease, request, result, cancelled, deadline)) return true;
            error = GetLastError();
            Retire(*lease.slot);
            if (error == ERROR_CANCELLED || error == ERROR_TIMEOUT || error == ERROR_INVALID_DATA ||
                lease.slot->poisoned) break;
            if (Cancelled(cancelled, deadline)) { error = GetLastError(); break; }
        }
    } catch (const std::bad_alloc&) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        if (lease.slot) Retire(*lease.slot);
    } catch (...) {
        error = ERROR_GEN_FAILURE;
        if (lease.slot) Retire(*lease.slot);
    }
    result = {};
    result.error = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
    return false;
}

} // namespace pulse::ipc
