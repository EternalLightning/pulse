#include "operation_elevation_transport.h"

#include <shellapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::ops {
namespace {
constexpr uint32_t kMagic = 0x454c5550; // PULE
constexpr uint32_t kVersion = 1;
constexpr DWORD kConnectTimeout = 30000;
constexpr DWORD kWriteTimeout = 10000;
constexpr DWORD kCancelTimeout = 15000;
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\Pulse.OperationElevation.";
enum class FrameKind : uint32_t { Request = 1, Progress = 2, Terminal = 3, Cancel = 4, Control = 5, TerminalAck = 6 };

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
struct LocalAllocation {
    void* value = nullptr;
    ~LocalAllocation() { if (value) LocalFree(value); }
};
HRESULT WinError(DWORD error = GetLastError()) {
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
std::wstring ProcessPath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size);
    return path;
}
std::wstring CanonicalPath(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file) return {};
    std::wstring result(32768, L'\0');
    DWORD size = GetFinalPathNameByHandleW(file.value, result.data(),
        static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!size || size >= result.size()) return {};
    result.resize(size);
    return result;
}
bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), -1,
        b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring Sibling(const std::wstring& path, const wchar_t* name) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash + 1) + name;
}
std::vector<uint8_t> ProcessUser(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return {};
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return {};
    std::vector<uint8_t> storage(size);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), size, &size)) return {};
    const auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    if (!IsValidSid(user->User.Sid)) return {};
    std::vector<uint8_t> sid(GetLengthSid(user->User.Sid));
    if (!CopySid(static_cast<DWORD>(sid.size()), sid.data(), user->User.Sid)) return {};
    return sid;
}
bool SameUser(HANDLE first, HANDLE second) {
    auto a = ProcessUser(first);
    auto b = ProcessUser(second);
    return !a.empty() && !b.empty() && EqualSid(a.data(), b.data());
}
bool Elevated(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    TOKEN_ELEVATION elevated{};
    DWORD size = 0;
    if (!GetTokenInformation(token.value, TokenElevation, &elevated,
        sizeof(elevated), &size) || !elevated.TokenIsElevated) return false;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<uint8_t> storage(size);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, storage.data(), size, &size)) return false;
    auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count && *GetSidSubAuthority(label->Label.Sid, count - 1) >= SECURITY_MANDATORY_HIGH_RID;
}
bool ExpectedPaths(const std::wstring& parent_path, const std::wstring& worker_path, bool testing) {
    const std::wstring parent = CanonicalPath(parent_path);
    const std::wstring worker = CanonicalPath(worker_path);
#ifdef PULSE_OPERATION_ELEVATION_TESTING
    if (testing) return EqualPath(parent, worker);
#else
    (void)testing;
#endif
    return EqualPath(parent, CanonicalPath(Sibling(worker, L"Pulse.exe"))) &&
        EqualPath(worker, CanonicalPath(Sibling(parent, L"Pulse.Shell.exe")));
}
std::wstring MakePipeName() {
    std::array<uint8_t, 32> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    std::wstring name = kPipePrefix;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto byte : random) {
        name.push_back(hex[byte >> 4]);
        name.push_back(hex[byte & 15]);
    }
    return name;
}
bool ValidPipeName(const std::wstring& name) {
    constexpr size_t prefix_size = std::size(kPipePrefix) - 1;
    if (name.size() != prefix_size + 64 || name.compare(0, prefix_size, kPipePrefix) != 0) return false;
    return std::all_of(name.begin() + prefix_size, name.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}
bool MakeSecurity(LocalAllocation& descriptor) {
    const auto sid = ProcessUser(GetCurrentProcess());
    if (sid.empty()) return false;
    LocalAllocation text;
    if (!ConvertSidToStringSidW(const_cast<uint8_t*>(sid.data()),
        reinterpret_cast<LPWSTR*>(&text.value))) return false;
    // FILE_GENERIC_WRITE includes FILE_CREATE_PIPE_INSTANCE: grant individual
    // client write rights instead, so the user ACE cannot create a second server.
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0012019b;;;";
    sddl += static_cast<const wchar_t*>(text.value);
    sddl += L")";
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
        SDDL_REVISION_1, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE;
}
void PutU32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t GetU32(const uint8_t* bytes) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
    return value;
}
struct WaitContext {
    HANDLE process = nullptr;
    HANDLE stop = nullptr;
    void* context = nullptr;
    bool (*poll)(void*) = nullptr;
};
DWORD Await(HANDLE pipe, OVERLAPPED& pending, DWORD timeout, const WaitContext& wait,
            DWORD& transferred) {
    const ULONGLONG started = GetTickCount64();
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        if (GetOverlappedResult(pipe, &pending, &transferred, FALSE)) return ERROR_SUCCESS;
        error = GetLastError();
        if (error != ERROR_IO_INCOMPLETE) break;
        if (wait.poll && !wait.poll(wait.context)) { error = ERROR_CANCELLED; break; }
        if (wait.stop && WaitForSingleObject(wait.stop, 0) != WAIT_TIMEOUT) { error = ERROR_CANCELLED; break; }
        if (wait.process && WaitForSingleObject(wait.process, 0) != WAIT_TIMEOUT) { error = ERROR_BROKEN_PIPE; break; }
        if (timeout != INFINITE && GetTickCount64() - started >= timeout) { error = ERROR_TIMEOUT; break; }
        const DWORD result = WaitForSingleObject(pending.hEvent, 50);
        if (result == WAIT_FAILED) { error = GetLastError(); break; }
    }
    CancelIoEx(pipe, &pending);
    // The buffer and OVERLAPPED remain alive until cancellation is acknowledged.
    GetOverlappedResult(pipe, &pending, &transferred, TRUE);
    return error;
}
DWORD Transfer(HANDLE pipe, bool writing, uint8_t* bytes, size_t size,
               DWORD timeout, const WaitContext& wait = {}) {
    const ULONGLONG started = GetTickCount64();
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return GetLastError();
    while (size) {
        OVERLAPPED pending{};
        pending.hEvent = event.value;
        ResetEvent(event.value);
        const DWORD chunk = static_cast<DWORD>((std::min)(size, size_t{65536}));
        DWORD done = 0;
        const BOOL ok = writing ? WriteFile(pipe, bytes, chunk, &done, &pending) :
            ReadFile(pipe, bytes, chunk, &done, &pending);
        if (!ok) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) return error;
            DWORD remaining = INFINITE;
            if (timeout != INFINITE) {
                const ULONGLONG elapsed = GetTickCount64() - started;
                remaining = elapsed >= timeout ? 0 : timeout - static_cast<DWORD>(elapsed);
            }
            const DWORD waited = Await(pipe, pending, remaining, wait, done);
            if (waited != ERROR_SUCCESS) return waited;
        }
        if (!done || done > chunk) return ERROR_BROKEN_PIPE;
        bytes += done;
        size -= done;
    }
    return ERROR_SUCCESS;
}
DWORD WriteFrame(HANDLE pipe, FrameKind kind, std::span<const uint8_t> payload,
                 const WaitContext& wait = {}) {
    if (payload.size() > kOperationElevationMaxPayload) return ERROR_BAD_LENGTH;
    std::array<uint8_t, 16> header{};
    PutU32(header.data(), kMagic);
    PutU32(header.data() + 4, kVersion);
    PutU32(header.data() + 8, static_cast<uint32_t>(kind));
    PutU32(header.data() + 12, static_cast<uint32_t>(payload.size()));
    DWORD error = Transfer(pipe, true, header.data(), header.size(), kWriteTimeout, wait);
    if (!error && !payload.empty()) error = Transfer(pipe, true,
        const_cast<uint8_t*>(payload.data()), payload.size(), kWriteTimeout, wait);
    return error;
}
DWORD ReadFrame(HANDLE pipe, FrameKind& kind, std::vector<uint8_t>& payload,
                const WaitContext& wait = {}, DWORD timeout = INFINITE) {
    std::array<uint8_t, 16> header{};
    DWORD error = Transfer(pipe, false, header.data(), header.size(), timeout, wait);
    if (error) return error;
    const uint32_t type = GetU32(header.data() + 8);
    const uint32_t size = GetU32(header.data() + 12);
    if (GetU32(header.data()) != kMagic || GetU32(header.data() + 4) != kVersion ||
        type < static_cast<uint32_t>(FrameKind::Request) || type > static_cast<uint32_t>(FrameKind::TerminalAck) ||
        size > kOperationElevationMaxPayload ||
        ((type == static_cast<uint32_t>(FrameKind::Cancel) ||
          type == static_cast<uint32_t>(FrameKind::TerminalAck)) && size != 0) ||
        (type == static_cast<uint32_t>(FrameKind::Terminal) && size < 8)) return ERROR_INVALID_DATA;
    kind = static_cast<FrameKind>(type);
    payload.resize(size);
    return Transfer(pipe, false, payload.data(), payload.size(), timeout, wait);
}
bool ParseUnsigned(const wchar_t* text, uint64_t& value) {
    value = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(*text - L'0');
        if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
struct ParentWait {
    HANDLE pipe = nullptr;
    const OperationElevationCallbacks* callbacks = nullptr;
    std::atomic<bool> cancellation_sent{false};
    std::atomic<ULONGLONG> cancelled_at{0};
    std::atomic<DWORD> cancellation_error{ERROR_SUCCESS};
    std::mutex writer;
    DWORD Send(FrameKind kind, std::span<const uint8_t> payload, HANDLE process = nullptr) {
        std::lock_guard lock(writer);
        return WriteFrame(pipe, kind, payload, {process});
    }
    static bool Poll(void* opaque) {
        auto& self = *static_cast<ParentWait*>(opaque);
        if (!self.cancellation_sent.load() && self.callbacks->cancelled &&
            self.callbacks->cancelled(self.callbacks->context)) {
            std::lock_guard lock(self.writer);
            if (!self.cancellation_sent.load()) {
                self.cancelled_at.store(GetTickCount64());
                self.cancellation_sent.store(true);
                self.cancellation_error.store(WriteFrame(self.pipe, FrameKind::Cancel, {}));
            }
        }
        return !self.cancellation_error.load() && (!self.cancellation_sent.load() ||
            GetTickCount64() - self.cancelled_at.load() < kCancelTimeout);
    }
};
bool ContinueConnecting(void* opaque) {
    const auto& callbacks = *static_cast<const OperationElevationCallbacks*>(opaque);
    return !callbacks.cancelled || !callbacks.cancelled(callbacks.context);
}
OperationElevationResult RunSession(const std::wstring& helper_exe, HWND owner,
    std::span<const uint8_t> request, const OperationElevationCallbacks& callbacks, bool testing,
    bool& submitted) {
    OperationElevationResult result;
    if (request.empty() || request.size() > kOperationElevationMaxPayload) {
        result.error = E_INVALIDARG;
        return result;
    }
    if (!ExpectedPaths(ProcessPath(GetCurrentProcess()), helper_exe, testing)) {
        result.error = E_ACCESSDENIED;
        return result;
    }
    if (callbacks.cancelled && callbacks.cancelled(callbacks.context)) {
        result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result.cancelled = true;
        return result;
    }
    const std::wstring name = MakePipeName();
    LocalAllocation security;
    if (name.empty() || !MakeSecurity(security)) { result.error = WinError(); return result; }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.value, FALSE};
    Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes));
    if (!pipe) { result.error = WinError(); return result; }
    const uint64_t parent_created = CreationTime(GetCurrentProcess());
    if (!parent_created) { result.error = WinError(); return result; }
    const std::wstring parameters = L"--file-operation-worker " + name + L" " +
        std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(parent_created);
    Handle process;
#ifdef PULSE_OPERATION_ELEVATION_TESTING
    if (testing) {
        std::wstring command = L"\"" + helper_exe + L"\" " + parameters;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(helper_exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
            result.error = WinError();
            return result;
        }
        process.value = info.hProcess;
        CloseHandle(info.hThread);
    } else
#endif
    {
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        launch.hwnd = owner;
        launch.lpVerb = L"runas";
        launch.lpFile = helper_exe.c_str();
        launch.lpParameters = parameters.c_str();
        launch.nShow = SW_HIDE;
        if (!ShellExecuteExW(&launch) || !launch.hProcess) {
            const DWORD error = GetLastError();
            result.error = WinError(error);
            result.cancelled = error == ERROR_CANCELLED;
            return result;
        }
        process.value = launch.hProcess;
    }
    const DWORD launched_pid = GetProcessId(process.value);
    const uint64_t launched_created = CreationTime(process.value);
    if (!launched_pid || !launched_created || !SameUser(GetCurrentProcess(), process.value) ||
        !EqualPath(CanonicalPath(ProcessPath(process.value)), CanonicalPath(helper_exe)) ||
        (!testing && !Elevated(process.value))) {
        result.error = E_ACCESSDENIED;
        return result;
    }
    Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected) { result.error = WinError(); return result; }
    const ULONGLONG connect_started = GetTickCount64();
    for (;;) {
        OVERLAPPED pending{};
        pending.hEvent = connected.value;
        ResetEvent(connected.value);
        const BOOL ok = ConnectNamedPipe(pipe.value, &pending);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_PIPE_CONNECTED) error = ERROR_SUCCESS;
        else if (error == ERROR_IO_PENDING) {
            DWORD ignored = 0;
            const ULONGLONG elapsed = GetTickCount64() - connect_started;
            const DWORD remaining = elapsed >= kConnectTimeout ? 0 : kConnectTimeout - static_cast<DWORD>(elapsed);
            error = Await(pipe.value, pending, remaining,
                {process.value, nullptr, const_cast<OperationElevationCallbacks*>(&callbacks), &ContinueConnecting}, ignored);
        }
        if (error) {
            result.error = WinError(error);
            result.cancelled = error == ERROR_CANCELLED;
            return result;
        }
        ULONG client_pid = 0;
        if (GetNamedPipeClientProcessId(pipe.value, &client_pid) && client_pid == launched_pid) {
            Handle client(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, client_pid));
            if (client && CreationTime(client.value) == launched_created &&
                SameUser(GetCurrentProcess(), client.value) &&
                EqualPath(CanonicalPath(ProcessPath(client.value)), CanonicalPath(helper_exe))) break;
        }
        DisconnectNamedPipe(pipe.value);
        if (GetTickCount64() - connect_started >= kConnectTimeout) {
            result.error = E_ACCESSDENIED;
            return result;
        }
    }
    // Cancellation while consent was displayed is checked before dispatch.
    if (callbacks.cancelled && callbacks.cancelled(callbacks.context)) {
        WriteFrame(pipe.value, FrameKind::Cancel, {});
        result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result.cancelled = true;
        return result;
    }
    result.outcome_unknown = true;
    submitted = true;
    DWORD error = WriteFrame(pipe.value, FrameKind::Request, request, {process.value});
    if (error) { result.error = WinError(error); return result; }
    ParentWait parent_wait;
    parent_wait.pipe = pipe.value;
    parent_wait.callbacks = &callbacks;
    std::jthread cancellation_monitor;
    if (callbacks.cancelled) {
        cancellation_monitor = std::jthread([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                try {
                    if (!ParentWait::Poll(&parent_wait)) break;
                } catch (...) {
                    parent_wait.cancellation_error.store(ERROR_GEN_FAILURE);
                    parent_wait.Send(FrameKind::Cancel, {});
                    break;
                }
                Sleep(50);
            }
        });
    }
    WaitContext wait{process.value, nullptr, &parent_wait, &ParentWait::Poll};
    for (;;) {
        if (!ParentWait::Poll(&parent_wait)) {
            result.error = WinError(parent_wait.cancellation_error.load() ? parent_wait.cancellation_error.load() : ERROR_CANCELLED);
            result.cancelled = parent_wait.cancellation_sent.load();
            return result;
        }
        FrameKind kind{};
        std::vector<uint8_t> payload;
        error = ReadFrame(pipe.value, kind, payload, wait);
        if (error) {
            result.error = WinError(parent_wait.cancellation_error.load() ? parent_wait.cancellation_error.load() : error);
            result.cancelled = parent_wait.cancellation_sent.load();
            return result;
        }
        if (kind == FrameKind::Progress) {
            std::vector<uint8_t> control;
            if (callbacks.on_progress) control = callbacks.on_progress(callbacks.context, payload);
            error = parent_wait.Send(FrameKind::Control, control, process.value);
            if (error) { result.error = WinError(error); return result; }
        } else if (kind == FrameKind::Terminal && GetU32(payload.data() + 4) <= 1) {
            result.error = static_cast<HRESULT>(GetU32(payload.data()));
            result.cancelled = GetU32(payload.data() + 4) != 0;
            result.outcome_unknown = false;
            result.response.assign(payload.begin() + 8, payload.end());
            submitted = false;
            parent_wait.Send(FrameKind::TerminalAck, {}, process.value);
            return result;
        } else {
            parent_wait.Send(FrameKind::Cancel, {});
            result.error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            return result;
        }
    }
}
struct WorkerState {
    HANDLE pipe = nullptr;
    HANDLE parent = nullptr;
    HANDLE stop = nullptr;
    std::atomic<bool> cancelled{false};
    std::mutex writer;
    std::mutex progress;
    std::mutex control_mutex;
    std::condition_variable control_changed;
    bool awaiting_control = false;
    bool control_ready = false;
    bool terminal_wait = false;
    bool terminal_ack = false;
    bool disconnected = false;
    std::vector<uint8_t> control;
};
struct WorkerThreadGuard {
    HANDLE stop = nullptr;
    std::thread* reader = nullptr;
    ~WorkerThreadGuard() {
        SetEvent(stop);
        if (reader && reader->joinable()) reader->join();
    }
};
} // namespace

OperationElevationResult RunElevatedOperation(const std::wstring& helper_exe,
    HWND owner, std::span<const uint8_t> request, const OperationElevationCallbacks& callbacks) {
    bool submitted = false;
    try { return RunSession(helper_exe, owner, request, callbacks, false, submitted); }
    catch (...) { return {E_OUTOFMEMORY, false, submitted, {}}; }
}
#ifdef PULSE_OPERATION_ELEVATION_TESTING
bool OperationElevationPathsForTest(const std::wstring& parent, const std::wstring& worker, bool testing) {
    return ExpectedPaths(parent, worker, testing);
}
OperationElevationResult RunOperationElevationForTest(const std::wstring& helper_exe,
    std::span<const uint8_t> request, const OperationElevationCallbacks& callbacks) {
    bool submitted = false;
    try { return RunSession(helper_exe, nullptr, request, callbacks, true, submitted); }
    catch (...) { return {E_OUTOFMEMORY, false, submitted, {}}; }
}
#endif
bool OperationElevationWorkerChannel::ReportProgress(std::span<const uint8_t> snapshot,
                                                    std::vector<uint8_t>* control) {
    auto& state = *static_cast<WorkerState*>(state_);
    std::lock_guard progress_lock(state.progress);
    {
        std::lock_guard lock(state.control_mutex);
        if (state.disconnected || state.awaiting_control) return false;
        state.awaiting_control = true;
        state.control_ready = false;
    }
    DWORD error = ERROR_SUCCESS;
    {
        std::lock_guard lock(state.writer);
        error = WriteFrame(state.pipe, FrameKind::Progress, snapshot, {state.parent, state.stop});
    }
    if (error) {
        state.cancelled.store(true);
        std::lock_guard lock(state.control_mutex);
        state.awaiting_control = false;
        return false;
    }
    std::unique_lock lock(state.control_mutex);
    while (!state.control_ready && !state.disconnected && !IsCancelled())
        state.control_changed.wait_for(lock, std::chrono::milliseconds(50));
    const bool received = state.control_ready;
    if (received && control) *control = std::move(state.control);
    state.control.clear();
    if (received) {
        state.awaiting_control = false;
        state.control_ready = false;
    }
    return received;
}
bool OperationElevationWorkerChannel::IsCancelled() const {
    auto& state = *static_cast<WorkerState*>(state_);
    return state.cancelled.load() || WaitForSingleObject(state.parent, 0) != WAIT_TIMEOUT;
}
int RunOperationElevationWorker(int argc, wchar_t** argv, const OperationElevationExecutor& executor) try {
    if (argc != 5 || !argv || !argv[1] || std::wstring(argv[1]) != L"--file-operation-worker" ||
        !argv[2] || !executor.execute) return ERROR_INVALID_PARAMETER;
    const std::wstring name = argv[2];
    uint64_t pid = 0, created = 0;
    if (!ValidPipeName(name) || !ParseUnsigned(argv[3], pid) || pid > MAXDWORD ||
        !ParseUnsigned(argv[4], created)) return ERROR_INVALID_PARAMETER;
    Handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
    if (!parent || CreationTime(parent.value) != created ||
        WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT ||
        !SameUser(parent.value, GetCurrentProcess())) return ERROR_ACCESS_DENIED;
#ifdef PULSE_OPERATION_ELEVATION_TESTING
    const bool testing = true;
#else
    const bool testing = false;
#endif
    if (!ExpectedPaths(ProcessPath(parent.value), ProcessPath(GetCurrentProcess()), testing) ||
        (!testing && !Elevated(GetCurrentProcess()))) return ERROR_ACCESS_DENIED;
    // Omit FILE_APPEND_DATA/FILE_CREATE_PIPE_INSTANCE from desired rights.
    constexpr DWORD client_access = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
    Handle pipe(CreateFileW(name.c_str(), client_access, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe) return static_cast<int>(GetLastError());
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(pipe.value, &server_pid) || server_pid != pid ||
        CreationTime(parent.value) != created ||
        WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
    FrameKind kind{};
    std::vector<uint8_t> request;
    DWORD error = ReadFrame(pipe.value, kind, request, {parent.value}, kConnectTimeout);
    if (error) return static_cast<int>(error);
    if (kind == FrameKind::Cancel) return ERROR_CANCELLED;
    if (kind != FrameKind::Request || request.empty()) return ERROR_INVALID_DATA;
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop) return static_cast<int>(GetLastError());
    WorkerState state;
    state.pipe = pipe.value;
    state.parent = parent.value;
    state.stop = stop.value;
    OperationElevationWorkerChannel channel(&state);
    std::thread cancellation_reader([&] {
        try {
            bool cancel_received = false;
            for (;;) {
                FrameKind incoming{};
                std::vector<uint8_t> payload;
                const DWORD read_error = ReadFrame(pipe.value, incoming, payload, {parent.value, stop.value});
                std::lock_guard lock(state.control_mutex);
                if (read_error) {
                    state.disconnected = true;
                    if (WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT) state.cancelled.store(true);
                    state.control_changed.notify_all();
                    break;
                }
                if (incoming == FrameKind::Cancel && !cancel_received) {
                    cancel_received = true;
                    state.cancelled.store(true);
                } else if (incoming == FrameKind::Control && state.awaiting_control && !state.control_ready) {
                    state.control = std::move(payload);
                    state.control_ready = true;
                } else if (incoming == FrameKind::TerminalAck && state.terminal_wait) {
                    state.terminal_ack = true;
                    state.control_changed.notify_all();
                    break;
                } else {
                    state.disconnected = true;
                    state.cancelled.store(true);
                    state.control_changed.notify_all();
                    break;
                }
                state.control_changed.notify_all();
            }
        } catch (...) {
            std::lock_guard lock(state.control_mutex);
            state.disconnected = true;
            state.cancelled.store(true);
            state.control_changed.notify_all();
        }
    });
    WorkerThreadGuard reader_guard{stop.value, &cancellation_reader};
    OperationElevationResult result;
    try { result = executor.execute(executor.context, request, channel); }
    catch (...) { result.error = E_FAIL; }
    if (result.response.size() > kOperationElevationMaxPayload - 8) {
        result.error = HRESULT_FROM_WIN32(ERROR_BAD_LENGTH);
        result.response.clear();
    }
    std::vector<uint8_t> terminal(8 + result.response.size());
    PutU32(terminal.data(), static_cast<uint32_t>(result.error));
    PutU32(terminal.data() + 4, result.cancelled ? 1u : 0u);
    std::copy(result.response.begin(), result.response.end(), terminal.begin() + 8);
    {
        std::lock_guard lock(state.control_mutex);
        state.terminal_wait = true;
    }
    {
        std::lock_guard lock(state.writer);
        error = WriteFrame(pipe.value, FrameKind::Terminal, terminal, {parent.value});
    }
    if (!error) {
        const ULONGLONG started = GetTickCount64();
        std::unique_lock lock(state.control_mutex);
        while (!state.terminal_ack && !state.disconnected &&
            GetTickCount64() - started < kWriteTimeout)
            state.control_changed.wait_for(lock, std::chrono::milliseconds(50));
    }
    SetEvent(stop.value);
    cancellation_reader.join();
    return static_cast<int>(error);
} catch (...) {
    return ERROR_NOT_ENOUGH_MEMORY;
}
} // namespace pulse::ops
