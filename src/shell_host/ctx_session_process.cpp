#include "ctx_session_process.h"
#include "process_retirement.h"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace pulse::shell {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::vector<uint8_t> EmptyMenu(uint32_t session) {
    ipc::PayloadWriter writer;
    writer.PutU32(session); writer.PutU32(0); writer.PutU32(0); writer.PutStringArray({});
    return writer.data();
}

std::vector<uint8_t> Failure(HRESULT error) {
    ipc::PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(error)); writer.PutU32(0);
    writer.PutString(L"Context menu worker stopped or unavailable");
    return writer.data();
}
}

struct ContextSessionProcesses::Core : std::enable_shared_from_this<Core> {
    struct Session {
        Handle stop;
        Handle wake;
        Handle process;
        std::mutex mutex;
        uint32_t invoke = 0;
        std::vector<uint8_t> invoke_payload;
        bool invoked = false;
        bool query_completed = false;
        bool accepting = true;
        bool closing = false;
        uint32_t next_id = 0;
        std::vector<uint8_t> next_query;
        Session() {
            stop.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            wake.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
    };
    std::wstring executable;
    Reply reply;
    Handle job;
    mutable std::mutex mutex;
    std::map<uint32_t, std::shared_ptr<Session>> sessions;
    std::shared_ptr<Session> idle;
    size_t worker_count = 0;
    ProcessRetirements<kProcessLimit> retirements;
    ReusePolicy reuse;
    bool stopping = false;

    void ReapLocked() { worker_count -= retirements.Reap(); }

    Core(std::wstring file, Reply callback, ReusePolicy policy)
        : executable(std::move(file)), reply(std::move(callback)), reuse(policy) {
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // Programs launched by a menu command must outlive the menu worker.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (job.value && !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits))) {
            CloseHandle(job.value); job.value = nullptr;
        }
    }

    void Send(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mutex);
        if (!stopping) reply(type, id, payload);
    }

    static bool Write(HANDLE pipe, uint32_t type, uint32_t id,
                      const std::vector<uint8_t>& payload, HANDLE stop, DWORD timeout_ms = 1000) {
        ipc::MsgHeader header;
        header.type = type; header.request_id = id;
        header.payload_size = static_cast<uint32_t>(payload.size());
        ipc::PipeIoOptions options;
        options.pending_timeout_ms = timeout_ms; options.cancel_event = stop;
        return ipc::PipeWrite(pipe, &header, sizeof(header), options) &&
            ipc::PipeWrite(pipe, payload.data(), header.payload_size, options);
    }

    static bool Released(HANDLE pipe, uint32_t id, const std::shared_ptr<Session>& session) {
        ipc::PayloadWriter close;
        close.PutU32(id);
        if (!Write(pipe, ipc::REQ_CTX_CLOSE, id, close.data(), session->stop.value, 100)) return false;
        const auto deadline = GetTickCount64() + 500;
        while (GetTickCount64() < deadline &&
                WaitForSingleObject(session->stop.value, 0) == WAIT_TIMEOUT) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available < sizeof(ipc::MsgHeader)) {
                WaitForSingleObject(session->stop.value, 10);
                continue;
            }
            ipc::PipeIoOptions options;
            options.pending_timeout_ms = 100; options.cancel_event = session->stop.value;
            ipc::MsgHeader header{};
            if (!ipc::PipeRead(pipe, &header, sizeof(header), options) || header.magic != ipc::kMagic ||
                    header.payload_size > ipc::kMaxPayload) return false;
            std::vector<uint8_t> payload(header.payload_size);
            if (!ipc::PipeRead(pipe, payload.data(), header.payload_size, options)) return false;
            if (header.type == ipc::RSP_PONG && header.request_id == id && payload.empty()) return true;
            if (header.type != ipc::RSP_CTX_ITEMS || header.request_id != id) return false;
        }
        return false;
    }

    void Run(uint32_t id, std::shared_ptr<Session> session, std::vector<uint8_t> query) {
        Handle pipe;
        pipe.value = INVALID_HANDLE_VALUE;
        bool invoke_done = false;
        const auto stop = [&] { return WaitForSingleObject(session->stop.value, 0) != WAIT_TIMEOUT; };
        std::wstring command = L"\"" + executable + L"\" --context-worker " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(id);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!stop() && job.value && CreateProcessW(executable.c_str(), command.data(), nullptr,
                nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            session->process.value = process.hProcess;
            if (AssignProcessToJobObject(job.value, process.hProcess)) ResumeThread(process.hThread);
            else SetEvent(session->stop.value);
            CloseHandle(process.hThread);
            const auto connect_deadline = GetTickCount64() + 8000;
            const auto name = ContextWorkerPipeName(GetCurrentProcessId(), id);
            while (!stop() && GetTickCount64() < connect_deadline &&
                   WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (pipe.value != INVALID_HANDLE_VALUE) {
                    ULONG server = 0;
                    if (GetNamedPipeServerProcessId(pipe.value, &server) && server == process.dwProcessId) break;
                    CloseHandle(pipe.value); pipe.value = INVALID_HANDLE_VALUE;
                    SetEvent(session->stop.value);
                    break;
                }
                WaitForSingleObject(session->stop.value, 20);
            }
            uint32_t queries = 0;
            while (pipe.value != INVALID_HANDLE_VALUE && !stop()) {
                ++queries;
                bool sent_query = Write(pipe.value, ipc::REQ_CTX_QUERY, id, query, session->stop.value);
                invoke_done = false;
                std::vector<uint8_t> partial_items;
                uint32_t partial_count = 0;
                bool query_timed_out = false;
                ULONGLONG invoke_finished_at = 0;
                bool close_requested = false;
                const auto deadline = GetTickCount64() + kSessionLifetimeMs;
                const auto query_deadline = GetTickCount64() + kQueryTimeoutMs;
                ULONGLONG window_check_tick = 0;
                bool visible = false;
                while (sent_query && !stop()) {
                    {
                        std::lock_guard lock(session->mutex);
                        close_requested = session->closing;
                        if (close_requested) session->accepting = false;
                    }
                    if (close_requested) break;
                    const auto now = GetTickCount64();
                    if (!session->query_completed && !session->invoked && now >= query_deadline) {
                        if (partial_items.empty() || !partial_count) break;
                        // Finish the UI query even when another extension never
                        // returns. Keep ready handlers alive for exact invocation.
                        const uint32_t final_flags = 0;
                        std::memcpy(partial_items.data() + sizeof(uint32_t), &final_flags, sizeof(final_flags));
                        Send(ipc::RSP_CTX_ITEMS, id, partial_items);
                        session->query_completed = true;
                        query_timed_out = true;
                    }
                    if ((invoke_done || now >= deadline) &&
                        now >= window_check_tick) {
                        visible = ContextProcessHasVisibleWindows(process.dwProcessId);
                        window_check_tick = now + 100;
                    }
                    if (invoke_done && now - invoke_finished_at >= 750 && !visible) break;
                    if (now >= deadline && !visible) break;
                    uint32_t invoke = 0;
                    std::vector<uint8_t> payload;
                    {
                        std::lock_guard lock(session->mutex);
                        if (session->invoke && !session->invoked) {
                            invoke = session->invoke; payload = session->invoke_payload;
                            session->invoked = true;
                        }
                    }
                    if (invoke && !Write(pipe.value, ipc::REQ_CTX_INVOKE, invoke, payload, session->stop.value)) break;
                    DWORD available = 0;
                    if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) break;
                    if (available < sizeof(ipc::MsgHeader)) {
                        if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT) break;
                        const HANDLE events[]{session->stop.value, session->wake.value, process.hProcess};
                        WaitForMultipleObjects(3, events, FALSE, 100);
                        continue;
                    }
                    ipc::PipeIoOptions options;
                    options.pending_timeout_ms = 1000; options.cancel_event = session->stop.value;
                    ipc::MsgHeader header{};
                    if (!ipc::PipeRead(pipe.value, &header, sizeof(header), options) ||
                        header.magic != ipc::kMagic || header.payload_size > ipc::kMaxPayload) break;
                    payload.resize(header.payload_size);
                    if (!ipc::PipeRead(pipe.value, payload.data(), header.payload_size, options)) break;
                    if (header.type == ipc::RSP_CTX_ITEMS && header.request_id == id) {
                        ipc::PayloadReader reader(payload.data(), payload.size());
                        uint32_t session_id = 0, flags = 0, count = 0;
                        if (!reader.GetU32(session_id) || session_id != id || !reader.GetU32(flags) ||
                            !reader.GetU32(count)) break;
                        if (query_timed_out) continue;
                        session->query_completed = !(flags & ipc::CTX_ITEMS_PARTIAL);
                        if (!session->query_completed) { partial_items = payload; partial_count = count; }
                        Send(header.type, id, payload);
                    } else if (header.type == ipc::RSP_DONE) {
                        { std::lock_guard lock(session->mutex); invoke = session->invoke; }
                        if (!invoke || header.request_id != invoke) break;
                        Send(header.type, invoke, payload);
                        invoke_done = true;
                        invoke_finished_at = GetTickCount64();
                        { std::lock_guard lock(session->mutex); session->accepting = false; }
                    } else break;
                }
                bool can_reuse = close_requested && session->query_completed && !session->invoked &&
                    !query_timed_out && queries < reuse.max_queries && reuse.idle_timeout_ms && !stop() &&
                    !ContextProcessHasVisibleWindows(process.dwProcessId) && Released(pipe.value, id, session) &&
                    !ContextProcessHasVisibleWindows(process.dwProcessId);
                uint32_t invoke = 0;
                {
                    std::lock_guard lock(session->mutex);
                    session->accepting = false;
                    invoke = session->invoke;
                    can_reuse = can_reuse && !invoke;
                }
                if (!can_reuse) break;
                {
                    std::lock_guard lock(mutex);
                    if (stopping || idle) break;
                    sessions.erase(id);
                    idle = session;
                    ResetEvent(session->wake.value);
                }
                const HANDLE waits[]{session->stop.value, session->wake.value, process.hProcess};
                WaitForMultipleObjects(3, waits, FALSE, reuse.idle_timeout_ms);
                {
                    std::lock_guard lock(mutex);
                    // A racing query wins the idle deadline only after reserving
                    // this worker under the same admission lock.
                    if (idle == session) { idle.reset(); break; }
                    if (!session->next_id) break;
                    id = session->next_id;
                    query = std::move(session->next_query);
                    session->next_id = 0;
                    if (stopping || stop()) break;
                }
            }
        }
        { std::lock_guard lock(session->mutex); session->accepting = false; }
        const bool reaped = retirements.Retire(session->process.value);
        if (!session->query_completed) Send(ipc::RSP_CTX_ITEMS, id, EmptyMenu(id));
        uint32_t invoke = 0;
        { std::lock_guard lock(session->mutex); invoke = session->invoke; }
        if (invoke && !invoke_done) Send(ipc::RSP_DONE, invoke, Failure(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
        { std::lock_guard lock(mutex); sessions.erase(id); if (reaped) --worker_count; }
    }
};

ContextSessionProcesses::ContextSessionProcesses(std::wstring executable, Reply reply)
    : ContextSessionProcesses(std::move(executable), std::move(reply), ReusePolicy{}) {}
ContextSessionProcesses::ContextSessionProcesses(std::wstring executable, Reply reply, ReusePolicy reuse)
    : core_(std::make_shared<Core>(std::move(executable), std::move(reply), reuse)) {}
ContextSessionProcesses::~ContextSessionProcesses() { Stop(); }

void ContextSessionProcesses::Query(uint32_t id, std::vector<uint8_t> payload) {
    auto session = std::make_shared<Core::Session>();
    bool admitted = false;
    {
        std::lock_guard lock(core_->mutex);
        core_->ReapLocked();
        if (!core_->stopping && !core_->sessions.contains(id)) {
            if (core_->idle) {
                session = core_->idle;
                core_->idle.reset();
                {
                    std::lock_guard session_lock(session->mutex);
                    session->next_id = id;
                    session->next_query = std::move(payload);
                    session->query_completed = false;
                    session->accepting = true;
                    session->closing = false;
                }
                core_->sessions.emplace(id, session);
                SetEvent(session->wake.value);
                return;
            }
            if (core_->job.value && session->stop.value && session->wake.value &&
                    core_->worker_count < kProcessLimit) {
                core_->sessions.emplace(id, session); admitted = true;
                ++core_->worker_count;
            }
        }
    }
    if (!admitted) { core_->Send(ipc::RSP_CTX_ITEMS, id, EmptyMenu(id)); return; }
    try {
        std::thread([core = core_, id, session, payload = std::move(payload)]() mutable {
            core->Run(id, session, std::move(payload));
        }).detach();
    } catch (...) {
        { std::lock_guard lock(core_->mutex); core_->sessions.erase(id); --core_->worker_count; }
        core_->Send(ipc::RSP_CTX_ITEMS, id, EmptyMenu(id));
    }
}

void ContextSessionProcesses::Invoke(uint32_t id, uint32_t request, std::vector<uint8_t> payload) {
    bool accepted = false;
    {
        std::lock_guard lock(core_->mutex);
        const auto found = core_->sessions.find(id);
        if (found != core_->sessions.end()) {
            std::lock_guard session_lock(found->second->mutex);
            if (found->second->accepting && !found->second->closing && !found->second->invoke &&
                WaitForSingleObject(found->second->stop.value, 0) == WAIT_TIMEOUT) {
                found->second->invoke = request; found->second->invoke_payload = std::move(payload);
                SetEvent(found->second->wake.value);
                accepted = true;
            }
        }
    }
    if (!accepted) core_->Send(ipc::RSP_DONE, request, Failure(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)));
}

void ContextSessionProcesses::Close(uint32_t id) {
    std::lock_guard lock(core_->mutex);
    if (const auto found = core_->sessions.find(id); found != core_->sessions.end()) {
        std::lock_guard session_lock(found->second->mutex);
        found->second->closing = true;
        SetEvent(found->second->wake.value);
    }
}

void ContextSessionProcesses::Stop() {
    std::lock_guard lock(core_->mutex);
    core_->stopping = true;
    for (const auto& [_, session] : core_->sessions) SetEvent(session->stop.value);
    if (core_->idle) SetEvent(core_->idle->stop.value);
}

size_t ContextSessionProcesses::ActiveProcesses() const {
    std::lock_guard lock(core_->mutex);
    core_->ReapLocked();
    return core_->worker_count;
}

size_t ContextSessionProcesses::IdleProcesses() const {
    std::lock_guard lock(core_->mutex);
    return core_->idle ? 1 : 0;
}
} // namespace pulse::shell
