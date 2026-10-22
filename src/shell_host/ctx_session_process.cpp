#include "ctx_session_process.h"
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
    bool stopping = false;

    Core(std::wstring file, Reply callback) : executable(std::move(file)), reply(std::move(callback)) {
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
                      const std::vector<uint8_t>& payload, HANDLE stop) {
        ipc::MsgHeader header;
        header.type = type; header.request_id = id;
        header.payload_size = static_cast<uint32_t>(payload.size());
        ipc::PipeIoOptions options;
        options.pending_timeout_ms = 1000; options.cancel_event = stop;
        return ipc::PipeWrite(pipe, &header, sizeof(header), options) &&
            ipc::PipeWrite(pipe, payload.data(), header.payload_size, options);
    }

    void Run(uint32_t id, std::shared_ptr<Session> session, std::vector<uint8_t> query) {
        Handle pipe;
        pipe.value = INVALID_HANDLE_VALUE;
        const auto stop = [&] { return WaitForSingleObject(session->stop.value, 0) != WAIT_TIMEOUT; };
        bool sent_query = false, invoke_done = false;
        std::vector<uint8_t> partial_items;
        uint32_t partial_count = 0;
        bool query_timed_out = false;
        ULONGLONG invoke_finished_at = 0;
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
            if (pipe.value != INVALID_HANDLE_VALUE && !stop())
                sent_query = Write(pipe.value, ipc::REQ_CTX_QUERY, id, query, session->stop.value);
            const auto deadline = GetTickCount64() + kSessionLifetimeMs;
            const auto query_deadline = GetTickCount64() + kQueryTimeoutMs;
            ULONGLONG window_check_tick = 0;
            bool visible = false;
            while (sent_query && !stop()) {
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
        }
        { std::lock_guard lock(session->mutex); session->accepting = false; }
        if (session->process.value) {
            TerminateProcess(session->process.value, ERROR_CANCELLED);
            // Keep the admission slot occupied until the process really exits.
            while (WaitForSingleObject(session->process.value, 100) == WAIT_TIMEOUT) {}
        }
        if (!session->query_completed) Send(ipc::RSP_CTX_ITEMS, id, EmptyMenu(id));
        uint32_t invoke = 0;
        { std::lock_guard lock(session->mutex); invoke = session->invoke; }
        if (invoke && !invoke_done) Send(ipc::RSP_DONE, invoke, Failure(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
        { std::lock_guard lock(mutex); sessions.erase(id); }
    }
};

ContextSessionProcesses::ContextSessionProcesses(std::wstring executable, Reply reply)
    : core_(std::make_shared<Core>(std::move(executable), std::move(reply))) {}
ContextSessionProcesses::~ContextSessionProcesses() { Stop(); }

void ContextSessionProcesses::Query(uint32_t id, std::vector<uint8_t> payload) {
    auto session = std::make_shared<Core::Session>();
    bool admitted = false;
    {
        std::lock_guard lock(core_->mutex);
        if (!core_->stopping && core_->job.value && session->stop.value && session->wake.value &&
            core_->sessions.size() < kProcessLimit && !core_->sessions.contains(id)) {
            core_->sessions.emplace(id, session); admitted = true;
        }
    }
    if (!admitted) { core_->Send(ipc::RSP_CTX_ITEMS, id, EmptyMenu(id)); return; }
    try {
        std::thread([core = core_, id, session, payload = std::move(payload)]() mutable {
            core->Run(id, session, std::move(payload));
        }).detach();
    } catch (...) {
        { std::lock_guard lock(core_->mutex); core_->sessions.erase(id); }
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
            if (found->second->accepting && !found->second->invoke &&
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
    if (const auto found = core_->sessions.find(id); found != core_->sessions.end())
        SetEvent(found->second->stop.value);
}

void ContextSessionProcesses::Stop() {
    std::lock_guard lock(core_->mutex);
    core_->stopping = true;
    for (const auto& [_, session] : core_->sessions) SetEvent(session->stop.value);
}

size_t ContextSessionProcesses::ActiveProcesses() const {
    std::lock_guard lock(core_->mutex);
    return core_->sessions.size();
}
} // namespace pulse::shell
