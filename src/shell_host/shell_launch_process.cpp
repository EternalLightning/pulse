#include "shell_launch_process.h"
#include "ctx_session_process.h"
#include "process_retirement.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace pulse::shell {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct WorkerProcess : Handle {
    ProcessRetirements<ShellLaunchProcesses::kProcessLimit>& retirements;
    bool& pending;
    WorkerProcess(ProcessRetirements<ShellLaunchProcesses::kProcessLimit>& owner, bool& retirement_pending)
        : retirements(owner), pending(retirement_pending) {}
    void Reap() { if (value) pending = !retirements.Retire(value); }
    ~WorkerProcess() { Reap(); }
};

std::vector<uint8_t> Result(HRESULT error, bool cancelled, const wchar_t* detail) {
    ipc::PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(error));
    writer.PutU32(cancelled ? 1u : 0u);
    writer.PutString(detail);
    return writer.data();
}

uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

bool WriteFrame(HANDLE pipe, uint32_t type, uint32_t id,
                const std::vector<uint8_t>& payload, HANDLE stop) {
    ipc::MsgHeader header;
    header.type = type;
    header.request_id = id;
    header.payload_size = static_cast<uint32_t>(payload.size());
    ipc::PipeIoOptions options;
    options.pending_timeout_ms = 1000;
    options.cancel_event = stop;
    return ipc::PipeWrite(pipe, &header, sizeof(header), options) &&
        ipc::PipeWrite(pipe, payload.data(), header.payload_size, options);
}
}

struct ShellLaunchProcesses::Core : std::enable_shared_from_this<Core> {
    struct Session {
        Handle stop;
        std::atomic<bool> user_cancel{false};
        std::atomic<bool> dispatched{false};
        std::atomic<bool> completed{false};
        bool active = false; // protected by Core::mutex
        bool retirement_pending = false; // written only by this session's runner
        Session() { stop.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    };
    struct Work {
        uint32_t id = 0;
        std::shared_ptr<Session> session;
        std::vector<uint8_t> payload;
        DWORD owner_pid = 0;
        uint64_t owner_created = 0;
    };
    std::wstring executable;
    Reply reply;
    Handle job;
    Handle ui_process;
    DWORD ui_pid = 0;
    uint64_t ui_created = 0;
    mutable std::mutex mutex;
    std::condition_variable work_available;
    std::mutex shutdown_mutex;
    std::vector<std::thread> runners;
    std::deque<Work> waiting;
    std::map<uint32_t, std::shared_ptr<Session>> sessions;
    size_t active_count = 0;
    ProcessRetirements<kProcessLimit> retirements;
    bool stopping = false;

    void ReapLocked() { active_count -= retirements.Reap(); }

    Core(std::wstring file, Reply callback) : executable(std::move(file)), reply(std::move(callback)) {
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (job.value && !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits))) {
            CloseHandle(job.value);
            job.value = nullptr;
        }
    }

    void Send(uint32_t id, const std::vector<uint8_t>& payload) {
        std::lock_guard lock(mutex);
        if (!stopping) reply(ipc::RSP_SHELL_LAUNCH_DONE, id, payload);
    }

    void Run(uint32_t id, std::shared_ptr<Session> session, std::vector<uint8_t> payload,
             DWORD owner_pid, uint64_t owner_created) {
        ipc::ShellLaunchRequest request;
        if (!ipc::ReadShellLaunch(payload.data(), payload.size(), request)) {
            Send(id, Result(E_INVALIDARG, false, L"Invalid Shell request."));
            return;
        }
        const bool picker = request.kind == ipc::ShellLaunchKind::PickFile ||
            request.kind == ipc::ShellLaunchKind::PickFolder;
        // Consent runs on a separate desktop. Preserve waiting for the user's
        // answer; process death, explicit cancellation and shutdown still end it.
        const DWORD idle_timeout = request.kind == ipc::ShellLaunchKind::ElevatedWait ? INFINITE : kNoWindowTimeoutMs;
        WorkerProcess process(retirements, session->retirement_pending);
        Handle pipe;
        pipe.value = INVALID_HANDLE_VALUE;
        bool dispatched = false, completed = false;
        const auto stopped = [&] { return WaitForSingleObject(session->stop.value, 0) != WAIT_TIMEOUT; };
        std::wstring command = L"\"" + executable + L"\" --launch-worker " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(id);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        if (!stopped() && job.value && CreateProcessW(executable.c_str(), command.data(), nullptr,
                nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
            process.value = child.hProcess;
            if (AssignProcessToJobObject(job.value, child.hProcess)) ResumeThread(child.hThread);
            else TerminateProcess(process.value, ERROR_NOT_READY);
            CloseHandle(child.hThread);
            const auto name = ShellLaunchPipeName(GetCurrentProcessId(), id);
            const auto connect_deadline = GetTickCount64() + 8000;
            while (!stopped() && GetTickCount64() < connect_deadline &&
                   WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
                pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (pipe.value != INVALID_HANDLE_VALUE) {
                    ULONG peer = 0;
                    if (GetNamedPipeServerProcessId(pipe.value, &peer) && peer == child.dwProcessId) break;
                    CloseHandle(pipe.value);
                    pipe.value = INVALID_HANDLE_VALUE;
                    break;
                }
                WaitForSingleObject(session->stop.value, 20);
            }
            ipc::PayloadWriter envelope;
            envelope.PutU32(owner_pid);
            envelope.PutU64(owner_created);
            auto work = envelope.data();
            work.insert(work.end(), payload.begin(), payload.end());
            bool sent = false;
            if (pipe.value != INVALID_HANDLE_VALUE && !stopped()) {
                // A failed write may have reached the peer; never replay or claim no launch.
                dispatched = true;
                session->dispatched.store(true);
                sent = WriteFrame(pipe.value, ipc::REQ_SHELL_LAUNCH, id, work, session->stop.value);
            }
            auto last_activity = GetTickCount64();
            while (dispatched && sent && !stopped()) {
                DWORD available = 0;
                if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) break;
                if (available >= sizeof(ipc::MsgHeader)) {
                    ipc::PipeIoOptions options;
                    options.pending_timeout_ms = 1000;
                    options.cancel_event = session->stop.value;
                    ipc::MsgHeader header{};
                    if (!ipc::PipeRead(pipe.value, &header, sizeof(header), options) ||
                        header.magic != ipc::kMagic || header.request_id != id ||
                        header.payload_size > ipc::kMaxPayload) break;
                    std::vector<uint8_t> result(header.payload_size);
                    if (!ipc::PipeRead(pipe.value, result.data(), header.payload_size, options)) break;
                    if (header.type == ipc::RSP_SHELL_LAUNCH_DONE && !completed) {
                        ipc::PayloadReader reader(result.data(), result.size());
                        uint32_t error = 0, cancelled = 0;
                        std::wstring detail;
                        if (!reader.GetU32(error) || !reader.GetU32(cancelled) || cancelled > 1 ||
                            !reader.GetString(detail) || reader.remaining()) break;
                        completed = true;
                        session->completed.store(true);
                        Send(id, result);
                    } else if (header.type != kShellLaunchHeartbeat || header.payload_size != 0) break;
                    last_activity = GetTickCount64();
                }
                if (WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) break;
                // Modal UI can run without our message loop; never time out a visible dialog.
                if (ContextProcessHasVisibleWindows(child.dwProcessId, picker)) last_activity = GetTickCount64();
                if (idle_timeout != INFINITE && GetTickCount64() - last_activity >= idle_timeout) break;
                const HANDLE events[]{session->stop.value, process.value};
                WaitForMultipleObjects(2, events, FALSE, 50);
            }
        }
        process.Reap();
        if (!completed) {
            const bool cancelled = session->user_cancel.load();
            Send(id, Result(dispatched ? ipc::kShellLaunchOutcomeUnknown :
                HRESULT_FROM_WIN32(cancelled ? ERROR_CANCELLED : ERROR_NOT_READY), cancelled,
                dispatched ? L"Shell launch worker stopped; launch outcome is unknown. The request was not replayed." :
                    L"Shell launch worker unavailable before dispatch; no launch occurred."));
        }
    }

    void RunQueue() {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                for (;;) {
                    ReapLocked();
                    if (stopping || (!waiting.empty() && active_count < kProcessLimit)) break;
                    if (waiting.empty()) work_available.wait(lock);
                    else work_available.wait_for(lock, std::chrono::milliseconds(100));
                }
                if (stopping) return;
                work = std::move(waiting.front());
                waiting.pop_front();
                work.session->active = true;
                ++active_count;
            }
            try {
                Run(work.id, work.session, std::move(work.payload), work.owner_pid, work.owner_created);
            } catch (...) {
                // Run's process guard reaps the worker before stack unwinding reaches here.
                try {
                    if (!work.session->completed.load()) {
                        const bool dispatched = work.session->dispatched.load();
                        const bool cancelled = work.session->user_cancel.load();
                        Send(work.id, Result(dispatched ? ipc::kShellLaunchOutcomeUnknown :
                            HRESULT_FROM_WIN32(cancelled ? ERROR_CANCELLED : ERROR_NOT_READY), cancelled,
                            dispatched ? L"Shell launch worker failed; launch outcome is unknown. The request was not replayed." :
                                L"Shell launch worker failed before dispatch; no launch occurred."));
                    }
                } catch (...) {}
            }
            {
                std::lock_guard lock(mutex);
                sessions.erase(work.id);
                if (!work.session->retirement_pending) --active_count;
            }
        }
    }
};

ShellLaunchProcesses::ShellLaunchProcesses(std::wstring executable, Reply reply)
    : core_(std::make_shared<Core>(std::move(executable), std::move(reply))) {
    core_->runners.reserve(kProcessLimit);
    for (size_t index = 0; index < kProcessLimit; ++index) {
        try {
            core_->runners.emplace_back([core = core_] { core->RunQueue(); });
        } catch (...) { break; }
    }
}
ShellLaunchProcesses::~ShellLaunchProcesses() { Stop(); }

void ShellLaunchProcesses::SetUiProcess(DWORD process) {
    std::lock_guard lock(core_->mutex);
    if (!core_->sessions.empty() || core_->stopping) return;
    if (core_->ui_process.value) CloseHandle(core_->ui_process.value);
    core_->ui_process.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    core_->ui_pid = process;
    core_->ui_created = core_->ui_process.value ? CreationTime(core_->ui_process.value) : 0;
}

void ShellLaunchProcesses::Submit(uint32_t id, std::vector<uint8_t> payload) {
    ipc::ShellLaunchRequest request;
    if (!id || payload.size() > ipc::kMaxPayload - 12 ||
        !ipc::ReadShellLaunch(payload.data(), payload.size(), request)) {
        core_->Send(id, Result(E_INVALIDARG, false, L"Malformed Shell launch request; no launch occurred."));
        return;
    }
    auto session = std::make_shared<Core::Session>();
    bool admitted = false;
    HRESULT failure = HRESULT_FROM_WIN32(ERROR_NOT_READY);
    const wchar_t* detail = L"Shell launch workers unavailable; no launch occurred.";
    {
        std::lock_guard lock(core_->mutex);
        if (core_->sessions.contains(id)) {
            failure = E_INVALIDARG;
            detail = L"Duplicate Shell launch request ID; no launch occurred.";
        } else if (core_->waiting.size() >= kQueueLimit) {
            failure = HRESULT_FROM_WIN32(ERROR_BUSY);
            detail = L"Shell launch waiting queue limit reached; no launch occurred.";
        } else if (!core_->stopping && core_->job.value && session->stop.value && !core_->runners.empty()) {
            try {
                core_->sessions.emplace(id, session);
                core_->waiting.push_back(Core::Work{id, session, std::move(payload), core_->ui_pid, core_->ui_created});
                admitted = true;
            } catch (...) {
                core_->sessions.erase(id);
                failure = E_OUTOFMEMORY;
                detail = L"Cannot queue Shell launch request; no launch occurred.";
            }
        }
    }
    if (!admitted) {
        core_->Send(id, Result(failure, false, detail));
        return;
    }
    core_->work_available.notify_one();
}

bool ShellLaunchProcesses::Cancel(uint32_t request) {
    bool queued = false;
    {
        std::lock_guard lock(core_->mutex);
        const auto found = core_->sessions.find(request);
        if (found == core_->sessions.end()) return false;
        found->second->user_cancel.store(true);
        if (found->second->active) SetEvent(found->second->stop.value);
        else {
            const auto work = std::find_if(core_->waiting.begin(), core_->waiting.end(),
                [request](const Core::Work& candidate) { return candidate.id == request; });
            if (work != core_->waiting.end()) core_->waiting.erase(work);
            core_->sessions.erase(found);
            queued = true;
        }
    }
    if (queued) core_->Send(request, Result(HRESULT_FROM_WIN32(ERROR_CANCELLED), true,
        L"Shell launch cancelled while queued; no launch occurred."));
    return true;
}

void ShellLaunchProcesses::Stop() {
    std::lock_guard shutdown_lock(core_->shutdown_mutex);
    {
        std::lock_guard lock(core_->mutex);
        core_->stopping = true;
        for (const auto& work : core_->waiting) core_->sessions.erase(work.id);
        core_->waiting.clear();
        for (const auto& [_, session] : core_->sessions) SetEvent(session->stop.value);
    }
    core_->work_available.notify_all();
    for (auto& runner : core_->runners) if (runner.joinable()) runner.join();
}

size_t ShellLaunchProcesses::ActiveProcesses() const {
    std::lock_guard lock(core_->mutex);
    core_->ReapLocked();
    return core_->active_count;
}
} // namespace pulse::shell
