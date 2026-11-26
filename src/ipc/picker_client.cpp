// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#include "picker_client.h"
#include "picker_session.h"
#include "../common/picker_owner_guard.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse::ipc::picker {
namespace {
Message Terminal(const Message& request, Status status, DWORD error = ERROR_SUCCESS) {
    Response response;
    if (const auto* body = std::get_if<Request>(&request.body)) response.operation = body->operation;
    response.status = status;
    response.error_code = error;
    response.error_source = error ? ErrorSource::Win32 : ErrorSource::None;
    return {request.request_id, std::move(response)};
}
struct ClientGuard {
    bool& active;
    explicit ClientGuard(bool& value) : active(value) { active = true; }
    ~ClientGuard() { active = false; }
};

HWND ForegroundReturnTarget(HWND owner) {
    const HWND root = GetAncestor(owner, GA_ROOT);
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (GetWindowThreadProcessId(owner, &pid) && pid != GetCurrentProcessId() &&
        IsWindowVisible(owner) && !IsIconic(owner)) return owner;
    if ((!root || !IsWindowVisible(root)) &&
        GetWindowThreadProcessId(foreground, &pid) && pid == GetCurrentProcessId())
        return foreground;
    for (HWND candidate = root; candidate; candidate = GetWindow(candidate, GW_OWNER)) {
        if (!GetWindowThreadProcessId(candidate, &pid) || pid != GetCurrentProcessId()) break;
        if (IsWindowVisible(candidate) && !IsIconic(candidate)) return candidate;
    }
    return nullptr;
}

struct Child {
    PipeHandle job;
    PipeHandle process;
    HMODULE module = nullptr;
    ~Child() {
        // Closing this private job cannot terminate the caller or unrelated apps.
        job.Reset();
        if (process) WaitForSingleObject(process.Get(), 5000);
    }

    bool FinishInBackground() {
        auto pending = std::unique_ptr<Child>(new (std::nothrow) Child);
        if (!pending || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&WaitForHandoff), &pending->module)) return false;
        pending->job = std::move(job);
        pending->process = std::move(process);
        PipeHandle thread(CreateThread(nullptr, 0, WaitForHandoff, pending.get(), 0, nullptr));
        if (!thread) {
            job = std::move(pending->job);
            process = std::move(pending->process);
            FreeLibrary(pending->module);
            return false;
        }
        pending.release();
        return true;
    }

private:
    static DWORD WINAPI WaitForHandoff(void* state) {
        std::unique_ptr<Child> child(static_cast<Child*>(state));
        const auto module = child->module;
        // The caller must return from Show before it can re-enable its owner.
        WaitForSingleObject(child->process.Get(), 2000);
        child.reset();
        FreeLibraryAndExitThread(module, 0);
    }
};
class HostHandles {
public:
    ~HostHandles() { if (attributes_) DeleteProcThreadAttributeList(attributes_); }
    bool Initialize(HANDLE endpoint) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        PipeHandle token(raw);
        if (!DuplicateHandle(GetCurrentProcess(), token.Get(), GetCurrentProcess(), &inherited_[0],
                             TOKEN_QUERY, TRUE, 0)) return false;
        query_.Reset(inherited_[0]);
        inherited_[1] = endpoint;
        const auto handle_bytes = endpoint ? sizeof(inherited_) : sizeof(HANDLE);
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return false;
        storage_.resize(bytes);
        const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return false;
        attributes_ = list;
        return UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                         inherited_, handle_bytes, nullptr, nullptr) != FALSE;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return attributes_; }
    HANDLE Token() const { return query_.Get(); }
    void ReleaseToken() { query_.Reset(); }
private:
    std::vector<BYTE> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
    HANDLE inherited_[2]{};
    PipeHandle query_;
};
struct EventMailbox {
    PipeHandle available{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    PipeHandle replied{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    DialogEvent event;
    DialogEventReply reply{};
};

TransportResult AnswerEvent(Channel& channel, const RequestId& id, DialogEvent event,
                            EventMailbox& mailbox, HANDLE cancel) {
    {
        std::lock_guard lock(mailbox.mutex);
        mailbox.event = std::move(event);
    }
    if (!SetEvent(mailbox.available.Get())) return {GetLastError()};
    HANDLE handles[]{cancel, mailbox.replied.Get()};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) return {ERROR_CANCELLED};
    if (wait != WAIT_OBJECT_0 + 1) return {GetLastError()};
    DialogEventReply reply{};
    {
        std::lock_guard lock(mailbox.mutex);
        reply = mailbox.reply;
    }
    return channel.Send({id, reply}, {cancel, 5000});
}

Message Exchange(const std::wstring& executable, const std::wstring& desktop,
                 const Message& request, HWND owner, HWND return_target, HANDLE cancel, bool& return_foreground,
                 EventMailbox& mailbox) {
    Server server;
    auto transport = Server::Create(server);
    DWORD owner_pid = 0;
    if (owner) GetWindowThreadProcessId(owner, &owner_pid);
    const bool preconnect = owner_pid && owner_pid != GetCurrentProcessId();
    PipeHandle endpoint;
    if (transport && preconnect) transport = server.OpenChildEndpoint(endpoint);
    if (!transport) return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    HostHandles inherited;
    if (!inherited.Initialize(endpoint.Get())) return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    Child child;
    child.job.Reset(CreateJobObjectW(nullptr, nullptr));
    if (!child.job) return Terminal(request, Status::Failed, GetLastError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(child.job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return Terminal(request, Status::Failed, GetLastError());
    const auto address = preconnect ? L"handle:" + std::to_wstring(reinterpret_cast<uintptr_t>(endpoint.Get())) : server.Name();
    std::wstring command = L"\"" + executable + L"\" --picker-host " + address + L" " +
        std::to_wstring(GetCurrentProcessId());
    // Same-process callers can pass a hidden modal helper. The host uses their
    // visible return target for placement, while the client guards the actual owner.
    const HWND host_owner = !preconnect && return_target ? return_target : owner;
    command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(host_owner)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(inherited.Token()));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(desktop.c_str());
    startup.lpAttributeList = inherited.Get();
    const auto working_directory = std::filesystem::path(executable).parent_path().wstring();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, working_directory.c_str(), &startup.StartupInfo, &info))
        return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    inherited.ReleaseToken();
    endpoint.Reset();
    child.process.Reset(info.hProcess);
    PipeHandle thread(info.hThread);
    if (!AssignProcessToJobObject(child.job.Get(), child.process.Get())) {
        const DWORD error = GetLastError();
        TerminateProcess(child.process.Get(), error); // Only our still-suspended child.
        return Terminal(request, Status::Failed, error);
    }
    // Our caller is in the foreground; let the host's dialog take it over.
    AllowSetForegroundWindow(info.dwProcessId);
    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
        return Terminal(request, Status::Failed, GetLastError());
    Channel channel;
    transport = preconnect ? server.BindChild(child.process.Get(), channel)
                           : server.Accept(child.process.Get(), channel, {cancel, 10000});
    bool foreground_handoff = preconnect || return_target != nullptr;
    if (transport) transport = NegotiatePickerSession(channel, request, {cancel, 10000}, foreground_handoff);
    if (!transport && transport.error != ERROR_CANCELLED)
        return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    if (transport) transport = channel.Send(request, {cancel, 10000});
    if (!transport && transport.error != ERROR_CANCELLED)
        return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    Message response;
    uint32_t serial = 0;
    while (transport) {
        transport = channel.Receive(response, {cancel, INFINITE});
        if (!transport) break;
        auto* event = std::get_if<DialogEvent>(&response.body);
        if (!event) break;
        if (response.request_id != request.request_id || event->serial != ++serial ||
            event->window > (std::numeric_limits<uintptr_t>::max)() ||
            !ValidateDialogEvent(std::get<Request>(request.body), *event))
            return Terminal(request, Status::Failed, ERROR_INVALID_DATA);
        DWORD window_pid = 0;
        if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<uintptr_t>(event->window)), &window_pid)) {
            // A user cancel can destroy the picker after it has sent an event.
            transport = channel.Receive(response, {cancel, 5000});
            break;
        }
        if (window_pid != info.dwProcessId)
            return Terminal(request, Status::Failed, ERROR_INVALID_WINDOW_HANDLE);
        transport = AnswerEvent(channel, request.request_id, std::move(*event), mailbox, cancel);
    }
    if (!transport && transport.error == ERROR_CANCELLED &&
        channel.Send({request.request_id, Cancel{}}, {nullptr, 1000}))
        WaitForSingleObject(child.process.Get(), 4000);
    if (transport && !ValidateResponse(request, response)) return Terminal(request, Status::Failed, ERROR_INVALID_DATA);
    if (transport && foreground_handoff) {
        const auto status = std::get<Response>(response.body).status;
        if ((status == Status::Accepted || status == Status::Cancelled) && child.FinishInBackground())
            return response;
    }
    // The foreground grant also applies when a live dialog is cancelled by its caller.
    if (WaitForSingleObject(child.process.Get(), transport ? 2000 : 0) == WAIT_OBJECT_0) {
        DWORD status = 0;
        return_foreground = GetExitCodeProcess(child.process.Get(), &status) && status == kPickerHostReturnForeground;
    }
    const auto& input = std::get<Request>(request.body);
    if (!transport && transport.error == ERROR_BROKEN_PIPE && !serial &&
        (input.required_capabilities & DialogEvents))
        return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    if (!transport) return Terminal(request, transport.error == ERROR_CANCELLED ? Status::Cancelled : Status::Failed,
                                    transport.error == ERROR_CANCELLED ? ERROR_SUCCESS : transport.error);
    return response;
}
}

Message RunPickerClient(std::wstring executable, Message request, HWND owner, HANDLE cancel,
                        const DialogEventHandler& on_event) {
    static thread_local bool active = false;
    if (active) return Terminal(request, Status::Failed, ERROR_BUSY);
    ClientGuard call(active);
    std::vector<uint8_t> encoded;
    const auto* body = std::get_if<Request>(&request.body);
    if (!body || bool(body->required_capabilities & DialogEvents) != bool(on_event) ||
        Encode(request, encoded) != CodecError::None)
        return Terminal(request, Status::Failed, ERROR_INVALID_DATA);
    if (owner && !IsWindow(owner))
        return Terminal(request, Status::Failed, ERROR_INVALID_WINDOW_HANDLE);
    if (executable.find(L'"') != std::wstring::npos || !std::filesystem::path(executable).is_absolute())
        return Terminal(request, Status::Failed, ERROR_INVALID_NAME);
    if (cancel) {
        const DWORD state = WaitForSingleObject(cancel, 0);
        if (state == WAIT_OBJECT_0) return Terminal(request, Status::Cancelled);
        if (state != WAIT_TIMEOUT) return Terminal(request, Status::Failed, ERROR_INVALID_HANDLE);
    }
    wchar_t desktop[32768]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &needed))
        return Terminal(request, Status::Failed, GetLastError());
    PipeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    PipeHandle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop || !done) return Terminal(request, Status::Failed, GetLastError());
    EventMailbox mailbox;
    if (on_event && (!mailbox.available || !mailbox.replied)) return Terminal(request, Status::Failed, GetLastError());
    Message response = Terminal(request, Status::Failed, ERROR_GEN_FAILURE);
    bool return_foreground = false;
    const HWND return_target = ForegroundReturnTarget(owner);
    pulse::PickerOwnerGuard modality(owner);
    if (modality.Error()) return Terminal(request, Status::Unsupported, ERROR_NOT_SUPPORTED);
    if (!modality.Alive()) return Terminal(request, Status::Cancelled);
    std::thread worker;
    try {
        worker = std::thread([&] {
            try { response = Exchange(executable, desktop, request, owner, return_target, stop.Get(), return_foreground, mailbox); }
            catch (...) { response = Terminal(request, Status::Failed, ERROR_NOT_ENOUGH_MEMORY); }
            SetEvent(done.Get());
        });
    } catch (const std::system_error&) { return Terminal(request, Status::Failed, ERROR_NOT_ENOUGH_MEMORY); }
    bool quit = false;
    int quit_code = 0;
    DWORD pump_error = ERROR_SUCCESS;
    bool cancelling = false;
    for (;;) {
        if (quit) {
            WaitForSingleObject(done.Get(), INFINITE); // Worker cleanup never requires caller UI dispatch.
            break;
        }
        HANDLE handles[]{done.Get(), mailbox.available.Get(), cancel};
        DWORD count = on_event ? 2 : 1;
        if (cancel && !cancelling) handles[count++] = cancel;
        const DWORD wait = MsgWaitForMultipleObjectsEx(count, handles, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) { pump_error = GetLastError(); SetEvent(stop.Get()); cancelling = true; }
        if ((cancel && !cancelling && wait == WAIT_OBJECT_0 + count - 1) || !modality.Alive()) {
            SetEvent(stop.Get()); cancelling = true;
        }
        if (on_event && !cancelling && wait == WAIT_OBJECT_0 + 1) {
            DialogEvent event;
            {
                std::lock_guard lock(mailbox.mutex);
                event = std::move(mailbox.event);
            }
            DialogEventReply reply{event.serial, static_cast<uint32_t>(E_FAIL)};
            try { reply = on_event(event); } catch (...) {}
            {
                std::lock_guard lock(mailbox.mutex);
                mailbox.reply = reply;
            }
            SetEvent(mailbox.replied.Get());
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true; quit_code = static_cast<int>(message.wParam);
                SetEvent(stop.Get()); cancelling = true;
                break;
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (WaitForSingleObject(done.Get(), 0) == WAIT_OBJECT_0) break;
        }
    }
    worker.join();
    // done may race cancellation, owner destruction or an already queued quit.
    MSG pending_quit{};
    if (!quit && PeekMessageW(&pending_quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        quit = true;
        quit_code = static_cast<int>(pending_quit.wParam);
    }
    if (quit) PostQuitMessage(quit_code);
    if (return_foreground && !quit && !pump_error) modality.RestoreForeground(return_target);
    if (pump_error) return Terminal(request, Status::Failed, pump_error);
    if (cancelling || quit || !modality.Alive() ||
        (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0))
        return Terminal(request, Status::Cancelled);
    return response;
}
} // namespace pulse::ipc::picker
