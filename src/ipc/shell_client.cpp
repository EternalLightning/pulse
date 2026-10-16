// shell_client.cpp — See shell_client.h for the contract.
#include "shell_client.h"
#include "file_identity_protocol.h"
#include <algorithm>
#include <utility>

namespace pulse::ipc {

namespace {
bool CanReplay(uint32_t type) {
    return type == REQ_PING || type == REQ_CTX_QUERY;
}

HRESULT DisconnectResult(uint32_t type) {
    return type == REQ_SHELL_LAUNCH ? kShellLaunchOutcomeUnknown : HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
}

const wchar_t* DisconnectMessage(uint32_t type) {
    if (type == REQ_AUTHORIZED_DELETE || type == REQ_AUTHORIZED_RECYCLE)
        return L"Deletion outcome is unknown; the destructive request was not replayed.";
    if (type == REQ_SHELL_LAUNCH || type == REQ_CTX_INVOKE)
        return L"Shell 操作结果未知；为避免重复执行，未自动重试。";
    return CanReplay(type) ? L"shell host died twice on one request"
                           : L"Shell operation outcome is unknown; the request was not replayed.";
}
}

ShellClient& ShellClient::Instance() {
    static ShellClient inst;
    return inst;
}

ShellClient::CallbackOwner::~CallbackOwner() {
    client->ReleaseOwner(*this);
}

ShellClient::Owner ShellClient::Acquire(Callbacks cb) {
    auto owner = std::make_shared<CallbackOwner>(this, std::move(cb));
    std::lock_guard lock(owners_mutex_);
    Start({});
    ++owners_;
    owner->attached = true;
    return owner;
}

void ShellClient::Release(const Owner& owner) {
    if (owner) ReleaseOwner(*owner);
}

void ShellClient::ReleaseOwner(CallbackOwner& owner) {
    if (!owner.active.exchange(false)) return;
    {
        // Wait for an in-flight callback before its window state is destroyed.
        std::lock_guard lock(owner.mutex);
        owner.callbacks = {};
    }
    std::lock_guard lock(owners_mutex_);
    if (owner.attached && --owners_ == 0) Stop();
}

void ShellClient::BindOwner(uint32_t id, const Owner& owner) {
    if (!owner) return;
    std::lock_guard lock(request_owners_mutex_);
    request_owners_.emplace(id, owner);
}

void ShellClient::Dispatch(uint32_t id, bool terminal, const std::function<void(Callbacks&)>& invoke) {
    std::weak_ptr<CallbackOwner> weak_owner;
    bool scoped = false;
    {
        std::lock_guard lock(request_owners_mutex_);
        if (const auto found = request_owners_.find(id); found != request_owners_.end()) {
            scoped = true;
            weak_owner = found->second;
            if (terminal) request_owners_.erase(found);
        }
    }
    if (!scoped) {
        invoke(cb_);
    } else if (const auto owner = weak_owner.lock()) {
        std::lock_guard lock(owner->mutex);
        if (owner->active.load()) invoke(owner->callbacks);
    }
}

ShellClient::~ShellClient() {
    Stop();
}

ShellClient::Connection::Connection(HANDLE handle)
    : pipe(handle), cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

ShellClient::Connection::~Connection() {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (cancel) CloseHandle(cancel);
}

std::shared_ptr<ShellClient::Connection> ShellClient::CurrentConnection() {
    std::lock_guard lock(connection_mutex_);
    return connection_;
}

void ShellClient::SignalConnection() {
    std::lock_guard lock(connection_mutex_);
    if (connection_) SetEvent(connection_->cancel);
}

// Caller holds send_mutex_; active I/O retains its own Connection ownership.
void ShellClient::RetireConnection() {
    {
        std::lock_guard lock(connection_mutex_);
        if (connection_) SetEvent(connection_->cancel);
        connection_.reset();
    }
    KillChild();
}

void ShellClient::Start(Callbacks cb) {
    if (running_.exchange(true)) return;
    cb_ = std::move(cb);
    reader_ = std::thread([this] { ReaderThread(); });
}

void ShellClient::Stop() {
    if (!running_.exchange(false)) return;
    SignalConnection();
    if (reader_.joinable()) reader_.join();
    std::map<uint32_t, Pending> stopped;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        RetireConnection();
        std::lock_guard pending_lock(pending_mutex_);
        stopped.swap(pending_);
    }
    for (const auto& [id, pending] : stopped)
        FireRequestDone(pending.type, id, HRESULT_FROM_WIN32(ERROR_CANCELLED), true, L"Shell client stopped.");
    std::map<uint32_t, LaunchReply> replies;
    {
        std::lock_guard lock(launch_reply_mutex_);
        replies.swap(launch_replies_);
    }
    for (auto& [id, reply] : replies) {
        (void)id;
        reply(HRESULT_FROM_WIN32(ERROR_CANCELLED), true, L"Shell client stopped.");
    }
}

void ShellClient::KillChild() {
    if (child_started_) {
        TerminateProcess(child_.hProcess, 1);
        CloseHandle(child_.hProcess);
        CloseHandle(child_.hThread);
        child_ = {};
        child_started_ = false;
    }
}

bool ShellClient::SpawnChild() {
    KillChild();

    const ULONGLONG now = GetTickCount64();
    if (last_spawn_try_ != 0 && now - last_spawn_try_ < 1500) return false;
    last_spawn_try_ = now;

    wchar_t exe_dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_dir, ARRAYSIZE(exe_dir));
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';

    std::wstring cmd = L"\"";
    cmd += exe_dir;
    cmd += L"Pulse.Shell.exe\" ";
    cmd += std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const DWORD gle = GetLastError();
        fprintf(stderr, "[shell_client] CreateProcess failed gle=%lu cmd=%ls\n", gle, cmd.c_str());
        if (gle == ERROR_FILE_NOT_FOUND) {
            last_error_ = L"\u627E\u4E0D\u5230 Pulse.Shell.exe\uFF08\u9700\u4E0E Pulse \u653E\u5728\u540C\u4E00\u76EE\u5F55\uFF09";
        } else {
            last_error_ = L"\u65E0\u6CD5\u542F\u52A8 Pulse.Shell.exe\uFF08\u9519\u8BEF " +
                          std::to_wstring(gle) + L"\uFF09";
        }
        return false;
    }
    child_ = pi;
    child_started_ = true;
    last_error_.clear();
    return true;
}

// Caller must hold send_mutex_.
bool ShellClient::EnsureConnected() {
    if (!running_.load()) return false;
    if (const auto connection = CurrentConnection()) {
        if (WaitForSingleObject(connection->cancel, 0) == WAIT_TIMEOUT) return true;
        RetireConnection();
    }

    std::wstring name = PipeNameFor(GetCurrentProcessId());
    if (!child_started_ && !SpawnChild()) return false;
    HANDLE h = INVALID_HANDLE_VALUE;
    // WaitNamedPipe returns ERROR_FILE_NOT_FOUND before the child creates its
    // pipe, so retry in a bounded loop.
    const auto deadline = GetTickCount64() + 8000;
    while (true) {
        if (!running_.load()) return false;
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            if (GetNamedPipeServerProcessId(h, &server_pid) &&
                server_pid == child_.dwProcessId)
                break;
            CloseHandle(h);
            last_error_ = L"Pulse.Shell.exe pipe identity mismatch";
            return false;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(name.c_str(), 200);
            if (GetTickCount64() > deadline) {
                last_error_ = L"Pulse.Shell.exe \u7BA1\u9053\u5FD9\u788C";
                return false;
            }
            continue;
        }
        if (err == ERROR_FILE_NOT_FOUND) {
            if (WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                GetExitCodeProcess(child_.hProcess, &code);
                fprintf(stderr, "[shell_client] child exited early code=%lu\n", code);
                last_error_ = L"Pulse.Shell.exe \u542F\u52A8\u540E\u7ACB\u5373\u9000\u51FA";
                KillChild();
                return false;
            }
            if (GetTickCount64() > deadline) {
                last_error_ = L"Pulse.Shell.exe \u672A\u54CD\u5E94\u7BA1\u9053";
                return false;
            }
            Sleep(50);
            continue;
        }
        fprintf(stderr, "[shell_client] pipe CreateFile err=%lu\n", err);
        return false;
    }
    auto connection = std::make_shared<Connection>(h);
    if (!connection->cancel) return false;
    {
        std::lock_guard lock(connection_mutex_);
        if (!running_.load()) return false;
        connection_ = std::move(connection);
    }
    return true;
}

// Caller must hold send_mutex_.
bool ShellClient::SendFrame(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    if (!running_.load()) return false;
    const auto connection = CurrentConnection();
    if (!connection) return false;
    MsgHeader h;
    h.type = type;
    h.request_id = id;
    h.payload_size = (uint32_t)payload.size();

    PipeIoOptions options;
    options.cancel_event = connection->cancel;
    options.deadline = GetTickCount64() + 5000;
    auto write_all = [&](const void* data, DWORD size) {
        bool ok = PipeWrite(connection->pipe, static_cast<const uint8_t*>(data), size, options);
        if (!ok)
            fprintf(stderr, "[client] WriteFile size=%lu failed gle=%lu\n", size, GetLastError());
        return ok;
    };

    if (!write_all(&h, sizeof(h)) ||
        (!payload.empty() && !write_all(payload.data(), (DWORD)payload.size()))) {
        SetEvent(connection->cancel); // A partial frame cannot be reused.
        return false;
    }
    return true;
}

uint32_t ShellClient::Submit(uint32_t type, const std::vector<uint8_t>& payload,
                             const std::function<void(uint32_t)>& before_send, const Owner& owner) {
    if (!running_.load() || (owner && !owner->active.load())) return 0;

    Pending p;
    p.type = type;
    p.payload = payload;
    p.id = next_id_.fetch_add(1);
    BindOwner(p.id, owner);
    if (before_send) before_send(p.id);

    bool sent = false;
    std::shared_ptr<Connection> connection;
    std::wstring unavailable_error;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (EnsureConnected()) {
            connection = CurrentConnection();
            {
                std::lock_guard pending_lock(pending_mutex_);
                pending_.emplace(p.id, p);
            }
            sent = SendFrame(p.type, p.id, p.payload);
        }
        if (!sent && !connection) unavailable_error = UnreachableMessage();
    }
    if (!sent && connection) HandleDisconnect(connection);
    else if (!sent) FireRequestDone(type, p.id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                                    unavailable_error);
    return p.id;
}

uint32_t ShellClient::DeleteAuthorized(const std::vector<std::wstring>& paths, uint64_t token,
                                      bool recycle, HWND owner, const std::vector<FileIdentity>& expected_identities,
                                      const Owner& callbacks) {
    if (!running_.load() || paths.empty() || token == 0 ||
        (callbacks && !callbacks->active.load())) return 0;
    if (!expected_identities.empty() && (expected_identities.size() != paths.size() ||
        std::any_of(expected_identities.begin(), expected_identities.end(), [](const auto& id) { return !id.Valid(); }))) return 0;
    Pending pending;
    pending.id = next_id_.fetch_add(1);
    BindOwner(pending.id, callbacks);
    pending.type = recycle ? REQ_AUTHORIZED_RECYCLE : REQ_AUTHORIZED_DELETE;
    pending.retried = true; // Never replay a destructive request on a new host.
    bool sent = false;
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        FILETIME created{}, exited{}, kernel{}, user{};
        if (EnsureConnected() && GetProcessTimes(child_.hProcess, &created, &exited, &kernel, &user)) {
            ULARGE_INTEGER epoch{};
            epoch.LowPart = created.dwLowDateTime; epoch.HighPart = created.dwHighDateTime;
            PayloadWriter writer;
            writer.PutU64(epoch.QuadPart); writer.PutU64(token); writer.PutStringArray(paths);
            if (!expected_identities.empty()) WriteFileIdentities(writer, expected_identities);
            if (recycle) writer.PutU64(reinterpret_cast<uintptr_t>(owner));
            pending.payload = writer.data();
            connection = CurrentConnection();
            {
                std::lock_guard pending_lock(pending_mutex_);
                pending_.emplace(pending.id, pending);
            }
            sent = SendFrame(pending.type, pending.id, pending.payload);
        }
    }
    if (!sent && connection) HandleDisconnect(connection);
    else if (!sent) FireDone(pending.id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false,
                        L"Deletion outcome is unknown; the destructive request was not replayed.");
    return pending.id;
}

uint32_t ShellClient::CreateFolder(const std::wstring& path, const Owner& owner) {
    PayloadWriter w;
    w.PutString(path);
    return Submit(REQ_NEW_FOLDER, w.data(), {}, owner);
}

uint32_t ShellClient::CreateNewFile(const std::wstring& path, const Owner& owner) {
    PayloadWriter w;
    w.PutString(path);
    return Submit(REQ_NEW_FILE, w.data(), {}, owner);
}

uint32_t ShellClient::Launch(const ShellLaunchRequest& request, LaunchReply reply, const Owner& owner) {
    return Submit(REQ_SHELL_LAUNCH, WriteShellLaunch(request),
        reply ? std::function<void(uint32_t)>([this, reply = std::move(reply)](uint32_t id) mutable {
            std::lock_guard lock(launch_reply_mutex_);
            launch_replies_.emplace(id, std::move(reply));
        }) : std::function<void(uint32_t)>{}, owner);
}

void ShellClient::Cancel(uint32_t id) {
    std::unique_lock lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        // The frame writer owns the stream. Interrupt it rather than appending
        // a cancellation frame into a partially written request.
        SignalConnection();
        return;
    }
    const auto connection = CurrentConnection();
    if (!connection) return;
    const bool sent = SendFrame(REQ_CANCEL, id, {});
    lock.unlock();
    if (!sent) HandleDisconnect(connection);
}

void ShellClient::Abort(uint32_t id) {
    SignalConnection();
    uint32_t type = 0;
    {
        std::lock_guard pending_lock(pending_mutex_);
        const auto found = pending_.find(id);
        if (found != pending_.end()) {
            type = found->second.type;
            pending_.erase(found);
        }
    }
    if (type) FireRequestDone(type, id, DisconnectResult(type), false, DisconnectMessage(type));
}

uint32_t ShellClient::QueryContextMenu(const std::vector<std::wstring>& paths,
                                       uint32_t owner_hwnd, bool background, bool extended,
                                        const std::vector<std::wstring>& disabled_clsids,
                                        const std::function<void(uint32_t)>& before_send, const Owner& owner) {
    PayloadWriter w;
    w.PutU32(owner_hwnd);
    uint32_t flags = 0;
    if (extended) flags |= CTXF_EXTENDED;
    if (background) flags |= CTXF_BACKGROUND;
    w.PutU32(flags);
    w.PutStringArray(paths);
    w.PutStringArray(disabled_clsids);
    return Submit(REQ_CTX_QUERY, w.data(), before_send, owner);
}

uint32_t ShellClient::InvokeContextMenu(uint32_t session_id, uint32_t item_id,
                                         const std::wstring& verb, const std::wstring& text,
                                         const std::function<void(uint32_t)>& before_send, const Owner& owner) {
    PayloadWriter w;
    w.PutU32(session_id);
    w.PutU32(item_id);
    w.PutString(verb);
    w.PutString(text);
    return Submit(REQ_CTX_INVOKE, w.data(), before_send, owner);
}

// Fire-and-forget like Cancel: no pending entry, no retry-on-restart (a fresh
// host has no such session anyway).
void ShellClient::CloseContextMenu(uint32_t session_id) {
    PayloadWriter w;
    w.PutU32(session_id);
    std::lock_guard<std::mutex> lock(send_mutex_);
    {
        std::lock_guard pending_lock(pending_mutex_);
        pending_.erase(session_id);
    }
    { std::lock_guard owners_lock(request_owners_mutex_); request_owners_.erase(session_id); }
    if (!CurrentConnection()) return;
    SendFrame(REQ_CTX_CLOSE, 0, w.data());
}

bool ShellClient::Ping() {
    return Submit(REQ_PING, {}) != 0;
}

void ShellClient::FireDone(uint32_t id, uint32_t hr, bool cancelled, const std::wstring& error) {
    Dispatch(id, true, [&](Callbacks& cb) {
        if (cb.done) cb.done(id, hr, cancelled, error);
    });
}

void ShellClient::FireRequestDone(uint32_t type, uint32_t id, uint32_t hr, bool cancelled,
                                  const std::wstring& error) {
    if (type == REQ_SHELL_LAUNCH) {
        LaunchReply reply;
        {
            std::lock_guard lock(launch_reply_mutex_);
            if (const auto found = launch_replies_.find(id); found != launch_replies_.end()) {
                reply = std::move(found->second);
                launch_replies_.erase(found);
            }
        }
        Dispatch(id, true, [&](Callbacks& cb) {
            if (reply) reply(static_cast<HRESULT>(hr), cancelled, error);
            else if (cb.launch_done) cb.launch_done(id, hr, cancelled, error);
        });
    } else FireDone(id, hr, cancelled, error);
}

std::wstring ShellClient::UnreachableMessage() const {
    static const std::wstring kFallback = L"shell host unreachable";
    return last_error_.empty() ? kFallback : last_error_;
}

void ShellClient::ReaderThread() {
    while (running_.load()) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (EnsureConnected()) connection = CurrentConnection();
        }
        if (!connection) {
            for (int i = 0; i < 30 && running_.load(); ++i) Sleep(50);
            continue;
        }

        while (running_.load()) {
            MsgHeader h{};
            PipeIoOptions options;
            options.cancel_event = connection->cancel;
            // Idle is unbounded, but a peer cannot hold a half-frame forever.
            if (!PipeRead(connection->pipe, &h, 1, options)) break;
            options.deadline = GetTickCount64() + 5000;
            if (!PipeRead(connection->pipe, reinterpret_cast<uint8_t*>(&h) + 1,
                          sizeof(h) - 1, options)) break;
            if (h.magic != kMagic || h.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(h.payload_size);
            if (before_frame_body_) before_frame_body_();
            if (h.payload_size && !PipeRead(connection->pipe, payload.data(), h.payload_size, options)) break;

            PayloadReader r(payload.data(), payload.size());
            if (h.type == RSP_PROGRESS) {
                float pct = 0.0f;
                std::wstring item;
                uint32_t items_done = 0, total_items = 0;
                if (r.GetF32(pct) && r.GetString(item) &&
                    r.GetU32(items_done) && r.GetU32(total_items)) {
                    Dispatch(h.request_id, false, [&](Callbacks& cb) {
                        if (cb.progress) cb.progress(h.request_id, pct, item, items_done, total_items);
                    });
                }
            } else if (h.type == RSP_SHELL_LAUNCH_DONE) {
                uint32_t hr = 0, cancelled = 0;
                std::wstring error;
                if (!r.GetU32(hr) || !r.GetU32(cancelled) || cancelled > 1 ||
                    !r.GetString(error) || r.remaining()) break;
                bool pending = false;
                {
                    std::lock_guard lock(pending_mutex_);
                    const auto found = pending_.find(h.request_id);
                    if (found != pending_.end() && found->second.type == REQ_SHELL_LAUNCH) {
                        pending_.erase(found);
                        pending = true;
                    }
                }
                if (pending) FireRequestDone(REQ_SHELL_LAUNCH, h.request_id, hr, cancelled != 0, error);
            } else if (h.type == RSP_DONE) {
                bool pending = false, deletion = false;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    const auto found = pending_.find(h.request_id);
                    if (found != pending_.end() && found->second.type != REQ_SHELL_LAUNCH) {
                        deletion = found->second.type == REQ_AUTHORIZED_DELETE || found->second.type == REQ_AUTHORIZED_RECYCLE;
                        if (deletion) pending_.erase(found);
                        pending = true;
                    }
                }
                if (pending && deletion) {
                    DeleteResultPayload result;
                    if (!ReadDeleteResultPayload(r, result)) {
                        result.hr = static_cast<uint32_t>(kMalformedDeleteResult);
                        result.error = L"Malformed deletion result";
                    }
                    Dispatch(h.request_id, true, [&](Callbacks& cb) {
                        if (cb.delete_done)
                            cb.delete_done(h.request_id, result.hr, result.cancelled, std::move(result.error),
                                std::move(result.deleted_paths), std::move(result.recycled_paths),
                                std::move(result.recycle_destinations), std::move(result.failed_path));
                        else if (cb.done) cb.done(h.request_id, result.hr, result.cancelled, result.error);
                    });
                } else if (pending) {
                    uint32_t hr = 0, cancelled = 0;
                    std::wstring err;
                    if (r.GetU32(hr) && r.GetU32(cancelled) && r.GetString(err)) {
                        std::vector<FileIdentity> produced;
                        if (r.remaining() && (!ReadFileIdentities(r, produced) || r.remaining())) {
                            hr = static_cast<uint32_t>(E_INVALIDARG);
                            err = L"Malformed produced-item identity result";
                            produced.clear();
                        }
                        { std::lock_guard<std::mutex> lock(pending_mutex_); pending_.erase(h.request_id); }
                        Dispatch(h.request_id, true, [&](Callbacks& cb) {
                            if (cb.produced_done && !produced.empty())
                                cb.produced_done(h.request_id, hr, cancelled != 0, std::move(err), std::move(produced));
                            else if (cb.done) cb.done(h.request_id, hr, cancelled != 0, err);
                        });
                    }
                }
            } else if (h.type == RSP_CTX_ITEMS) {
                uint32_t session = 0, msg_flags = 0, count = 0;
                std::vector<CtxMenuItem> items;
                bool ok = r.GetU32(session) && r.GetU32(msg_flags) && r.GetU32(count) &&
                          count <= 4096;
                const bool partial = (msg_flags & CTX_ITEMS_PARTIAL) != 0;
                for (uint32_t i = 0; ok && i < count; ++i) {
                    CtxMenuItem it;
                    uint32_t flags = 0;
                    ok = r.GetU32(it.id) && r.GetU32(flags) &&
                         r.GetString(it.verb) && r.GetString(it.text);
                    if (ok) {
                        if (!r.GetString(it.clsid)) it.clsid.clear();
                        if (!r.GetString(it.handler)) it.handler.clear();
                    }
                    it.enabled = (flags & CTX_ITEM_ENABLED) != 0;
                    it.separator_after = (flags & CTX_ITEM_SEPARATOR_AFTER) != 0;
                    it.has_children = (flags & CTX_ITEM_HAS_CHILDREN) != 0;
                    it.child = (flags & CTX_ITEM_CHILD) != 0;
                    if (ok) items.push_back(std::move(it));
                }
                std::vector<std::wstring> slow_clsids;
                if (ok) ok = r.TryStringArray(slow_clsids) && r.remaining() == 0;
                if (!ok) break;
                if (ok) {
                    bool pending = false;
                    {
                        std::lock_guard<std::mutex> lock(pending_mutex_);
                        if (partial) {
                            pending = pending_.contains(h.request_id);
                        } else {
                            pending = pending_.erase(h.request_id) != 0;
                        }
                    }
                    if (pending) Dispatch(h.request_id, !partial, [&](Callbacks& cb) {
                        if (cb.ctx_items) cb.ctx_items(h.request_id, std::move(items), partial,
                                                      std::move(slow_clsids));
                    });
                }
            } else if (h.type == RSP_PONG) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                pending_.erase(h.request_id);
            }
        }

        if (!running_.load()) break;
        HandleDisconnect(connection);
    }
}

// Only read-only requests may be replayed after losing a host instance.
void ShellClient::HandleDisconnect(const std::shared_ptr<Connection>& expected) {
    if (expected) SetEvent(expected->cancel);
    std::vector<Pending> retry;
    std::vector<Pending> failed;
    std::vector<std::pair<Pending, std::wstring>> unavailable;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        // A sender may already have retired the connection read by this reader.
        // It must not tear down the replacement or consume its pending requests.
        if (expected && (CurrentConnection() != expected || !running_.load())) return;
        RetireConnection();
        {
            std::lock_guard pending_lock(pending_mutex_);
            for (auto& [id, p] : pending_) {
                if (!CanReplay(p.type) || p.retried) failed.push_back(std::move(p));
                else { p.retried = true; retry.push_back(std::move(p)); }
            }
            pending_.clear();
        }
        for (auto& p : retry) {
            bool sent = false;
            if (EnsureConnected()) {
                {
                    std::lock_guard pending_lock(pending_mutex_);
                    pending_.emplace(p.id, p);
                }
                sent = SendFrame(p.type, p.id, p.payload);
                if (!sent) {
                    std::lock_guard pending_lock(pending_mutex_);
                    pending_.erase(p.id);
                }
            }
            if (!sent) unavailable.emplace_back(std::move(p), UnreachableMessage());
        }
    }
    for (const auto& p : failed)
        FireRequestDone(p.type, p.id, DisconnectResult(p.type), false, DisconnectMessage(p.type));
    for (const auto& [p, error] : unavailable)
        FireRequestDone(p.type, p.id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), false, error);
}

} // namespace pulse::ipc
