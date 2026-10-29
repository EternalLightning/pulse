#include "../ipc/shell_client.h"
#include "../ipc/file_identity_protocol.h"
#include <sddl.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace {
using namespace pulse::ipc;
int failures = 0;
constexpr DWORD kDeadlineMs = 5000;

void Check(bool condition, const char* label) {
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!condition) ++failures;
}

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return value_; }
    void reset(HANDLE value) { if (valid()) CloseHandle(value_); value_ = value; }
    HANDLE release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }
private:
    HANDLE value_;
};

class UserSecurity {
public:
    UserSecurity() {
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) { Fail("OpenProcessToken"); return; }
        Handle token(raw_token);
        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) { Fail("GetTokenInformation(size)"); return; }
        std::vector<BYTE> data(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, data.data(), bytes, &bytes)) {
            Fail("GetTokenInformation(user)"); return;
        }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) {
            Fail("ConvertSidToStringSidW"); return;
        }
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                &descriptor_, nullptr)) { Fail("ConvertStringSecurityDescriptorToSecurityDescriptorW"); return; }
        attributes_.nLength = static_cast<DWORD>(sizeof(attributes_));
        attributes_.lpSecurityDescriptor = descriptor_;
    }
    ~UserSecurity() { if (descriptor_) LocalFree(descriptor_); }
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }
    DWORD error() const { return error_; }
    const char* stage() const { return stage_; }
private:
    void Fail(const char* stage) { error_ = GetLastError(); stage_ = stage; }
    DWORD error_ = ERROR_SUCCESS;
    const char* stage_ = "security descriptor";
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

class PipePair {
public:
    bool Connect() {
        static uint32_t sequence = 0;
        UserSecurity security;
        if (!security.get()) return Failed(security.stage(), security.error());
        const std::wstring name = L"\\\\.\\pipe\\PulseShellLaunchFixture." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) +
            L"." + std::to_wstring(++sequence);
        server_.reset(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
            FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, security.get()));
        if (!server_.valid()) return Failed("CreateNamedPipeW", GetLastError());
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return Failed("CreateEventW", GetLastError());
        OVERLAPPED connected{};
        connected.hEvent = event.get();
        const BOOL immediate = ConnectNamedPipe(server_.get(), &connected);
        const DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
        if (!immediate && error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED)
            return Failed("ConnectNamedPipe", error);
        client_.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!client_.valid()) {
            const DWORD client_error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                CancelIoEx(server_.get(), &connected);
                DWORD ignored = 0;
                GetOverlappedResult(server_.get(), &connected, &ignored, TRUE);
            }
            return Failed("CreateFileW(client)", client_error);
        }
        if (error == ERROR_IO_PENDING) {
            const DWORD waited = WaitForSingleObject(event.get(), kDeadlineMs);
            if (waited != WAIT_OBJECT_0) {
                const DWORD wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
                CancelIoEx(server_.get(), &connected);
                DWORD ignored = 0;
                GetOverlappedResult(server_.get(), &connected, &ignored, TRUE);
                return Failed("WaitForSingleObject(connect)", wait_error);
            }
            DWORD ignored = 0;
            if (!GetOverlappedResult(server_.get(), &connected, &ignored, FALSE))
                return Failed("GetOverlappedResult(connect)", GetLastError());
        }
        return true;
    }
    HANDLE ReleaseClient() { return client_.release(); }
    void DropPeer() { server_.reset(INVALID_HANDLE_VALUE); }
    bool ReadHeader(MsgHeader& header) {
        PipeIoOptions options;
        options.pending_timeout_ms = kDeadlineMs;
        return PipeRead(server_.get(), &header, sizeof(header), options);
    }
    bool ReadRequest(uint32_t type, uint32_t id) {
        PipeIoOptions options;
        options.pending_timeout_ms = kDeadlineMs;
        MsgHeader header;
        if (!PipeRead(server_.get(), &header, static_cast<DWORD>(sizeof(header)), options) || header.magic != kMagic ||
            header.payload_size > kMaxPayload) return false;
        std::vector<uint8_t> payload(header.payload_size);
        return PipeRead(server_.get(), payload.data(), header.payload_size, options) &&
            header.type == type && header.request_id == id;
    }
    bool WriteRaw(const void* data, DWORD size) {
        PipeIoOptions options;
        options.deadline = GetTickCount64() + kDeadlineMs;
        return PipeWrite(server_.get(), data, size, options);
    }
    bool Respond(uint32_t type, uint32_t id, HRESULT hr = S_OK, const std::wstring& error = {}) {
        PayloadWriter writer;
        writer.PutU32(static_cast<uint32_t>(hr));
        writer.PutU32(0);
        writer.PutString(error);
        return RespondPayload(type, id, writer);
    }
    bool RespondPayload(uint32_t type, uint32_t id, const PayloadWriter& writer) {
        MsgHeader header;
        header.type = type;
        header.request_id = id;
        header.payload_size = static_cast<uint32_t>(writer.data().size());
        PipeIoOptions options;
        options.pending_timeout_ms = kDeadlineMs;
        return PipeWrite(server_.get(), &header, static_cast<DWORD>(sizeof(header)), options) &&
            PipeWrite(server_.get(), writer.data().data(), header.payload_size, options);
    }
private:
    static bool Failed(const char* stage, DWORD error) {
        std::cout << "[DIAG] PipePair::Connect stage=" << stage << " win32=" << error <<
            " pid=" << GetCurrentProcessId() << '\n';
        return false;
    }
    Handle server_, client_;
};

struct Completion {
    uint32_t id, hr;
    bool launch, cancelled;
    std::wstring error;
};

class Recorder {
public:
    ShellClient::Callbacks Callbacks() {
        ShellClient::Callbacks callbacks;
        callbacks.done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
            Add({id, hr, false, cancelled, std::move(error)});
        };
        callbacks.launch_done = [this](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
            Add({id, hr, true, cancelled, std::move(error)});
        };
        return callbacks;
    }
    bool Wait(size_t count, DWORD timeout = kDeadlineMs) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::milliseconds(timeout),
            [&] { return events_.size() >= count; });
    }
    std::vector<Completion> Snapshot() {
        std::lock_guard lock(mutex_);
        return events_;
    }
private:
    void Add(Completion completion) {
        {
            std::lock_guard lock(mutex_);
            events_.push_back(std::move(completion));
        }
        changed_.notify_all();
    }
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Completion> events_;
};

ShellLaunchRequest LaunchRequest() {
    ShellLaunchRequest request;
    request.paths = {L"shell-launch-fixture-item"};
    return request;
}
}

namespace pulse::ipc {
struct ShellLaunchClientTestProbe {
    static void MultiwindowOwners() {
        Recorder first, second, fallback;
        ShellClient client;
        client.cb_ = fallback.Callbacks();
        const auto first_owner = std::make_shared<ShellClient::CallbackOwner>(&client, first.Callbacks());
        const auto second_owner = std::make_shared<ShellClient::CallbackOwner>(&client, second.Callbacks());
        client.BindOwner(101, first_owner);
        client.BindOwner(102, second_owner);
        client.FireRequestDone(REQ_SHELL_LAUNCH, 102, static_cast<uint32_t>(E_ACCESSDENIED), false,
            L"second window launch failed");
        client.FireDone(101, S_OK, false, {});
        const auto first_events = first.Snapshot();
        const auto second_events = second.Snapshot();
        Check(first_events.size() == 1 && first_events[0].id == 101 && !first_events[0].launch &&
            second_events.size() == 1 && second_events[0].id == 102 && second_events[0].launch &&
            second_events[0].hr == static_cast<uint32_t>(E_ACCESSDENIED) &&
            second_events[0].error == L"second window launch failed" && fallback.Snapshot().empty(),
            "interleaved window completions and launch errors reach only their requesting owners");
        client.BindOwner(103, first_owner);
        client.BindOwner(104, second_owner);
        client.BindOwner(105, first_owner);
        client.Release(first_owner);
        client.FireDone(103, S_OK, false, {});
        client.FireRequestDone(REQ_SHELL_LAUNCH, 105, static_cast<uint32_t>(E_FAIL), false, L"late launch error");
        client.FireDone(104, S_OK, false, {});
        const auto surviving_events = second.Snapshot();
        Check(first.Snapshot().size() == 1 && surviving_events.size() == 2 &&
            surviving_events[1].id == 104 && fallback.Snapshot().empty() &&
            client.request_owners_.empty(),
            "closing one owner suppresses its late callback while the surviving owner completes normally");
        client.Release(second_owner);
    }
    static void MultiwindowOwnerFrames() {
        Recorder fallback;
        PipePair pair;
        ShellClient client;
        Check(pair.Connect(), "connect isolated multiwindow callback-routing fixture");
        const auto endpoint = pair.ReleaseClient();
        if (endpoint == INVALID_HANDLE_VALUE) return;
        Prepare(client, endpoint, fallback);
        std::mutex event_mutex;
        std::condition_variable changed;
        std::vector<std::pair<uint32_t, std::string>> events;
        bool payloads_valid = true;
        const auto record = [&](uint32_t id, std::string kind, bool valid) {
            {
                std::lock_guard lock(event_mutex);
                events.emplace_back(id, std::move(kind));
                payloads_valid = payloads_valid && valid;
            }
            changed.notify_all();
        };
        const auto callbacks = [&](const std::string& window) {
            ShellClient::Callbacks result;
            result.progress = [&, window](uint32_t id, float percent, std::wstring item,
                                           uint32_t done, uint32_t total) {
                record(id, window + ":progress", percent == 25.0f && item == L"item" && done == 1 && total == 4);
            };
            result.ctx_items = [&, window](uint32_t id, std::vector<CtxMenuItem> items, bool partial,
                                           std::vector<std::wstring> slow) {
                record(id, window + (partial ? ":partial-context" : ":context"), items.empty() && slow.empty());
            };
            result.delete_done = [&, window](uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                                             std::vector<std::wstring> deleted, std::vector<std::wstring> recycled,
                                             std::vector<std::wstring> destinations, std::wstring failed) {
                record(id, window + ":delete", hr == S_OK && !cancelled && error.empty() &&
                    deleted == std::vector<std::wstring>{L"deleted"} && recycled.empty() &&
                    destinations.empty() && failed.empty());
            };
            result.produced_done = [&, window](uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                                               std::vector<FileIdentity> identities) {
                record(id, window + ":produced", hr == S_OK && !cancelled && error.empty() &&
                    identities.size() == 1 && identities[0].Valid() && identities[0].value.VolumeSerialNumber == 7);
            };
            return result;
        };
        const auto first_owner = std::make_shared<ShellClient::CallbackOwner>(&client, callbacks("first"));
        const auto second_owner = std::make_shared<ShellClient::CallbackOwner>(&client, callbacks("second"));
        AddPending(client, REQ_AUTHORIZED_DELETE, 201);
        AddPending(client, REQ_CTX_QUERY, 202);
        AddPending(client, REQ_NEW_FILE, 203);
        client.BindOwner(201, first_owner);
        client.BindOwner(202, second_owner);
        client.BindOwner(203, second_owner);
        const auto wait_events = [&](size_t count) {
            std::unique_lock lock(event_mutex);
            return changed.wait_for(lock, std::chrono::milliseconds(kDeadlineMs), [&] { return events.size() >= count; });
        };
        Read(client);
        PayloadWriter progress, partial;
        progress.PutF32(25.0f); progress.PutString(L"item"); progress.PutU32(1); progress.PutU32(4);
        partial.PutU32(202); partial.PutU32(CTX_ITEMS_PARTIAL); partial.PutU32(0); partial.PutStringArray({});
        const bool partial_received = pair.RespondPayload(RSP_PROGRESS, 201, progress) &&
            pair.RespondPayload(RSP_CTX_ITEMS, 202, partial) && wait_events(2);
        bool bindings_retained = false;
        {
            std::lock_guard lock(client.request_owners_mutex_);
            bindings_retained = client.request_owners_.contains(201) && client.request_owners_.contains(202);
        }
        Check(partial_received && bindings_retained,
            "progress and partial context frames preserve their requesting window until terminal completion");
        PayloadWriter deletion, context, produced;
        deletion.PutU32(S_OK); deletion.PutU32(0); deletion.PutString({});
        deletion.PutStringArray({L"deleted"}); deletion.PutStringArray({}); deletion.PutStringArray({}); deletion.PutString({});
        context.PutU32(202); context.PutU32(0); context.PutU32(0); context.PutStringArray({});
        produced.PutU32(S_OK); produced.PutU32(0); produced.PutString({});
        FileIdentity identity;
        identity.value.VolumeSerialNumber = 7;
        identity.value.FileId.Identifier[0] = 1;
        WriteFileIdentities(produced, {identity});
        const bool terminal_received = pair.RespondPayload(RSP_DONE, 203, produced) &&
            pair.RespondPayload(RSP_CTX_ITEMS, 202, context) &&
            pair.RespondPayload(RSP_DONE, 201, deletion) && wait_events(5);
        client.Stop();
        const std::vector<std::pair<uint32_t, std::string>> expected{
            {201, "first:progress"}, {202, "second:partial-context"}, {203, "second:produced"},
            {202, "second:context"}, {201, "first:delete"}};
        Check(terminal_received && payloads_valid && events == expected && fallback.Snapshot().empty() &&
            client.request_owners_.empty() && Empty(client),
            "reader routes context, produced identities and deletion results to their owners and retires terminal bindings");
        client.Release(first_owner);
        client.Release(second_owner);
    }
    static void EarlyStartupExit() {
        ShellClient client;
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        Check(length != 0 && length < executable.size(), "locate isolated startup-exit child fixture");
        if (!length || length >= executable.size()) return;
        executable.resize(length);
        std::wstring command = L"\"" + executable + L"\" --exit-before-pipe";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        Check(started, "start test-owned child that exits without creating a Shell pipe");
        if (!started) return;
        client.child_ = process;
        client.child_started_ = true;
        client.running_.store(true);
        client.last_spawn_try_ = GetTickCount64();
        const bool exited = WaitForSingleObject(process.hProcess, kDeadlineMs) == WAIT_OBJECT_0;
        Check(exited, "test-owned startup child exits within deadline");
        if (!exited) { client.Stop(); return; }
        bool connected = false;
        {
            std::lock_guard lock(client.send_mutex_);
            connected = client.EnsureConnected();
        }
        Check(!connected && !client.child_started_ && !client.child_.hProcess &&
            !client.child_.hThread && !client.CurrentConnection(),
            "pre-pipe child exit retires startup state so later requests can spawn again");
        {
            std::lock_guard lock(client.send_mutex_);
            client.last_spawn_try_ = GetTickCount64();
            connected = client.EnsureConnected();
        }
        Check(!connected && !client.child_started_,
            "fresh startup attempts retain spawn backoff after early exit");
        client.Stop();
    }
    static void FailureMessageSnapshot() {
        ShellClient client;
        const std::wstring failed_attempt = L"connection attempt failed before callback dispatch";
        client.last_error_ = failed_attempt;
        AddPending(client, REQ_NEW_FILE, 81);
        AddPending(client, REQ_PING, 82);
        std::wstring reported;
        unsigned completed = 0;
        client.cb_.done = [&](uint32_t id, uint32_t, bool, std::wstring error) {
            ++completed;
            if (id == 81) {
                std::lock_guard lock(client.send_mutex_);
                client.last_error_ = L"a later connection attempt replaced the diagnostic";
            } else if (id == 82) reported = std::move(error);
        };
        // Fail the queued retry without creating a production helper. The first
        // callback replaces connection state before the retry callback is fired.
        client.HandleDisconnect();
        Check(completed == 2 && reported == failed_attempt && Empty(client),
            "failure callbacks run unlocked and keep their connection attempt's error snapshot");
    }
    static void Backpressure() {
        for (unsigned action = 0; action != 3; ++action) {
            Recorder recorder;
            PipePair pair;
            ShellClient client;
            Check(pair.Connect(), "connect live non-reading peer");
            const auto endpoint = pair.ReleaseClient();
            if (endpoint == INVALID_HANDLE_VALUE) return;
            Prepare(client, endpoint, recorder);
            Read(client);
            Handle writer_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            Handle action_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            uint32_t id = 0;
            std::thread writer([&] {
                client.Submit(REQ_NEW_FILE, std::vector<uint8_t>(1024 * 1024),
                    [&](uint32_t assigned) { id = assigned; });
                SetEvent(writer_done.get());
            });
            MsgHeader header;
            const bool blocked = pair.ReadHeader(header) && header.type == REQ_NEW_FILE &&
                WaitForSingleObject(writer_done.get(), 100) == WAIT_TIMEOUT;
            Check(blocked, "large frame blocks while peer remains alive without reading body");
            std::thread action_thread([&] {
                if (action == 0) client.Stop();
                else if (action == 1) client.Cancel(header.request_id);
                else client.Abort(header.request_id);
                SetEvent(action_done.get());
            });
            const bool bounded = WaitForSingleObject(action_done.get(), 1000) == WAIT_OBJECT_0;
            Check(bounded, action == 0 ? "Stop interrupts blocked submission without waiting for peer" :
                action == 1 ? "Cancel interrupts blocked submission without waiting for peer" :
                              "Abort interrupts blocked submission without waiting for peer");
            if (!bounded) pair.DropPeer();
            action_thread.join();
            Check(WaitForSingleObject(writer_done.get(), 2000) == WAIT_OBJECT_0,
                "cancelled write reclaims OVERLAPPED and returns within deadline");
            if (WaitForSingleObject(writer_done.get(), 0) != WAIT_OBJECT_0) pair.DropPeer();
            writer.join();
            client.Stop();
            const auto completions = recorder.Snapshot();
            Check(completions.size() == 1 && completions[0].id == id && Empty(client),
                "backpressure request receives exactly one terminal completion");
        }
    }
    static void MalformedResults() {
        std::vector<PayloadWriter> payloads(6);
        payloads[1].PutU32(S_OK);
        payloads[2].PutU32(S_OK); payloads[2].PutU32(1);
        payloads[3].PutU32(S_OK); payloads[3].PutU32(0); payloads[3].PutU32(UINT32_MAX);
        payloads[4].PutU32(S_OK); payloads[4].PutU32(2); payloads[4].PutString(L"");
        payloads[5].PutU32(S_OK); payloads[5].PutU32(1); payloads[5].PutString(L"");
        payloads[5].PutU32(1);
        for (const auto& payload : payloads) {
            Recorder recorder, fallback;
            PipePair pair;
            ShellClient client;
            Check(pair.Connect(), "P26 connect isolated malformed completion fixture");
            const auto endpoint = pair.ReleaseClient();
            if (endpoint == INVALID_HANDLE_VALUE) return;
            Prepare(client, endpoint, fallback);
            const auto owner = std::make_shared<ShellClient::CallbackOwner>(&client, recorder.Callbacks());
            for (uint32_t id : {71u, 72u, 73u}) {
                AddPending(client, REQ_NEW_FILE, id);
                client.BindOwner(id, owner);
            }
            Read(client);
            const bool received = pair.RespondPayload(RSP_DONE, 71, payload) &&
                pair.RespondPayload(RSP_DONE, 71, payload) && pair.Respond(RSP_DONE, 72) &&
                pair.Respond(RSP_DONE, 73) && recorder.Wait(3);
            const auto events = recorder.Snapshot();
            Check(received && events.size() == 3 && events[0].id == 71 &&
                events[0].hr == static_cast<uint32_t>(E_INVALIDARG) && !events[0].cancelled &&
                !events[0].error.empty() && events[1].id == 72 && events[1].hr == S_OK &&
                events[2].id == 73 && events[2].hr == S_OK && Empty(client) &&
                client.request_owners_.empty() && fallback.Snapshot().empty(),
                "P26 malformed terminal reply fails once, retires its owner and preserves subsequent frames");
            client.Stop();
            client.Release(owner);
        }
    }
    static void FrameLiveness() {
        for (unsigned mode = 0; mode != 3; ++mode) {
            Recorder recorder;
            PipePair pair;
            ShellClient client;
            Check(pair.Connect(), "connect isolated Shell half-frame fixture");
            const auto endpoint = pair.ReleaseClient();
            if (endpoint == INVALID_HANDLE_VALUE) return;
            Prepare(client, endpoint, recorder);
            AddPending(client, REQ_NEW_FILE, 77);
            Handle body_gate(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            Handle release_body(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            Handle stopped(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (mode == 0) client.before_frame_body_ = [&] {
                SetEvent(body_gate.get());
                WaitForSingleObject(release_body.get(), INFINITE);
            };
            const auto connection = client.CurrentConnection();
            Read(client);
            MsgHeader header;
            header.type = RSP_DONE;
            header.request_id = 77;
            header.payload_size = 12;
            const auto start = GetTickCount64();
            Check(pair.WriteRaw(&header, mode == 1 ? 1 : sizeof(header)),
                "peer supplies partial Shell frame and remains alive");
            if (mode == 0) {
                const bool gated = WaitForSingleObject(body_gate.get(), 1000) == WAIT_OBJECT_0;
                Check(gated, "Shell reaches deterministic gap before body admission");
                std::thread stop([&] { client.Stop(); SetEvent(stopped.get()); });
                const bool signalled = WaitForSingleObject(connection->cancel, 1000) == WAIT_OBJECT_0;
                SetEvent(release_body.get());
                const bool bounded = WaitForSingleObject(stopped.get(), 1000) == WAIT_OBJECT_0;
                Check(signalled && bounded, "Shell Stop forbids body I/O after durable cancellation");
                if (!bounded) pair.DropPeer();
                stop.join();
            } else {
                const bool terminal = recorder.Wait(1, 6500);
                const auto elapsed = GetTickCount64() - start;
                Check(terminal && elapsed >= 4500 && elapsed < 6500,
                    mode == 1 ? "Shell partial header expires under whole-frame budget" :
                                "Shell complete header does not grant unlimited body wait");
                client.Stop();
            }
            const auto events = recorder.Snapshot();
            Check(events.size() == 1 && events[0].id == 77 && Empty(client) && !client.reader_.joinable(),
                "Shell half-frame reclaims reader and completes request exactly once");
        }
    }
    static void RetiredStream() {
        Recorder recorder;
        PipePair pair;
        ShellClient client;
        Check(pair.Connect(), "connect isolated retired Shell stream fixture");
        const auto endpoint = pair.ReleaseClient();
        if (endpoint == INVALID_HANDLE_VALUE) return;
        Prepare(client, endpoint, recorder);
        const auto old = client.CurrentConnection();
        SetEvent(old->cancel);
        bool connected = false;
        {
            std::lock_guard lock(client.send_mutex_);
            connected = client.EnsureConnected();
        }
        Check(!connected && !client.CurrentConnection() && !client.child_started_,
            "a cancelled Shell stream is retired before attempting a fresh connection");
        Check(GetFileType(old->pipe) == FILE_TYPE_PIPE &&
            WaitForSingleObject(old->cancel, 0) == WAIT_OBJECT_0,
            "retired Shell connection stays owned until active users release it");
        client.Stop();
    }
    static void InteractionReplyRouting() {
        Recorder recorder;
        PipePair pair;
        ShellClient client;
        Check(pair.Connect(), "connect isolated picker reply fixture");
        const auto endpoint = pair.ReleaseClient();
        if (endpoint == INVALID_HANDLE_VALUE) return;
        Prepare(client, endpoint, recorder);
        Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        HRESULT result = E_FAIL;
        bool cancelled = false;
        std::wstring value;
        std::atomic<unsigned> count{0};
        const auto reply = [&](HRESULT error, bool cancel, std::wstring selected) {
            result = error;
            cancelled = cancel;
            value = std::move(selected);
            ++count;
            SetEvent(done.get());
        };
        const auto id = client.Launch(LaunchRequest(), reply);
        Check(pair.ReadRequest(REQ_SHELL_LAUNCH, id), "per-request picker reply is registered before dispatch");
        Read(client);
        Check(pair.Respond(RSP_SHELL_LAUNCH_DONE, id, S_OK, L"C:\\中文文件.txt") &&
            WaitForSingleObject(done.get(), kDeadlineMs) == WAIT_OBJECT_0 && result == S_OK &&
            !cancelled && value == L"C:\\中文文件.txt" && count == 1 && recorder.Snapshot().empty(),
            "picker selection bypasses ordinary launch error notification");
        ResetEvent(done.get());
        const auto pending = client.Launch(LaunchRequest(), reply);
        Check(pair.ReadRequest(REQ_SHELL_LAUNCH, pending), "leave custom interaction pending at shutdown");
        client.Stop();
        Check(WaitForSingleObject(done.get(), 0) == WAIT_OBJECT_0 && cancelled &&
            result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && count == 2 &&
            client.launch_replies_.empty() && recorder.Snapshot().empty(),
            "shutdown completes custom modal wait exactly once without stale selection");
    }
    static void Prepare(ShellClient& client, HANDLE pipe, Recorder& recorder) {
        client.cb_ = recorder.Callbacks();
        client.connection_ = std::make_shared<ShellClient::Connection>(pipe);
        // Suppress real helper creation even when an incorrect replay branch is entered.
        client.last_spawn_try_ = GetTickCount64();
        client.running_.store(true);
    }
    static void Read(ShellClient& client) {
        client.reader_ = std::thread([&client] { client.ReaderThread(); });
    }
    static bool Empty(ShellClient& client) {
        std::lock_guard lock(client.pending_mutex_);
        return client.pending_.empty();
    }
    static void AddPending(ShellClient& client, uint32_t type, uint32_t id, bool retried = false) {
        std::lock_guard lock(client.pending_mutex_);
        client.pending_.emplace(id, ShellClient::Pending{type, id, {}, retried});
    }
    static void FailedSubmission() {
        Recorder recorder;
        ShellClient client;
        Handle readonly(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        Check(readonly.valid(), "prepare non-writable transport handle");
        if (!readonly.valid()) return;
        Prepare(client, readonly.release(), recorder);
        const auto spawn_before = client.last_spawn_try_;
        const uint32_t id = client.Launch(LaunchRequest());
        const auto events = recorder.Snapshot();
        Check(id != 0 && events.size() == 1 && events[0].id == id && events[0].launch &&
            events[0].hr == static_cast<uint32_t>(kShellLaunchOutcomeUnknown) &&
            !events[0].cancelled && !events[0].error.empty(),
            "failed launch write completes once through launch_done with unknown outcome");
        Check(client.last_spawn_try_ == spawn_before && !client.child_started_ &&
            !client.CurrentConnection() && Empty(client),
            "failed launch submission leaves no replay or child restart state");
        client.Stop();
    }
    static void DisconnectPolicy() {
        Recorder recorder;
        ShellClient client;
        client.cb_ = recorder.Callbacks();
        AddPending(client, REQ_SHELL_LAUNCH, 21);
        AddPending(client, REQ_CTX_INVOKE, 22);
        AddPending(client, REQ_NEW_FILE, 23);
        AddPending(client, REQ_CTX_QUERY, 24);
        AddPending(client, REQ_PING, 25);
        AddPending(client, REQ_CTX_QUERY, 26, true);
        // Inspect disconnect classification without reconnecting or creating a helper.
        client.running_.store(false);
        client.HandleDisconnect();
        const auto events = recorder.Snapshot();
        const auto find = [&](uint32_t id) -> const Completion* {
            for (const auto& event : events) if (event.id == id) return &event;
            return nullptr;
        };
        const auto* launch = find(21);
        const auto* invoke = find(22);
        const auto* create = find(23);
        const uint32_t disconnected = static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE));
        Check(events.size() == 6 && launch && launch->launch &&
            launch->hr == static_cast<uint32_t>(kShellLaunchOutcomeUnknown) &&
            invoke && !invoke->launch && invoke->hr == disconnected &&
            create && !create->launch && create->hr == disconnected,
            "dispatched launch, context invoke and new-file fail without replay");
        const auto* query = find(24);
        const auto* ping = find(25);
        const auto* twice = find(26);
        const uint32_t unavailable = static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED));
        Check(query && query->hr == unavailable && ping && ping->hr == unavailable &&
            twice && twice->hr == disconnected && Empty(client) && !client.child_started_ &&
            client.last_spawn_try_ == 0,
            "only first read-only requests reach reconnect; exhausted query fails without spawn");
    }
    static void TransportRouting() {
        Recorder recorder;
        PipePair first, second;
        ShellClient client;
        const bool first_connected = first.Connect();
        Check(first_connected, "connect unique current-user-only OVERLAPPED fixture");
        if (!first_connected) return;
        HANDLE endpoint = first.ReleaseClient();
        if (endpoint == INVALID_HANDLE_VALUE) return;
        Prepare(client, endpoint, recorder);
        const uint32_t launch_a = client.Launch(LaunchRequest());
        const uint32_t ordinary_a = client.CreateNewFile(L"ordinary-fixture-a");
        const uint32_t launch_b = client.Launch(LaunchRequest());
        const uint32_t ordinary_b = client.InvokeContextMenu(ordinary_a, 1, L"fixture");
        const bool requests = first.ReadRequest(REQ_SHELL_LAUNCH, launch_a) &&
            first.ReadRequest(REQ_NEW_FILE, ordinary_a) &&
            first.ReadRequest(REQ_SHELL_LAUNCH, launch_b) &&
            first.ReadRequest(REQ_CTX_INVOKE, ordinary_b);
        Check(requests, "public requests reach the real fixture transport");
        if (!requests) return;
        Read(client);
        const bool responses = first.Respond(RSP_DONE, launch_a, E_FAIL, L"wrong type") &&
            first.Respond(RSP_SHELL_LAUNCH_DONE, launch_b, E_ACCESSDENIED, L"launch failure") &&
            first.Respond(RSP_DONE, ordinary_a) &&
            first.Respond(RSP_SHELL_LAUNCH_DONE, launch_b, E_FAIL, L"duplicate") &&
            first.Respond(RSP_SHELL_LAUNCH_DONE, launch_a) &&
            first.Respond(RSP_SHELL_LAUNCH_DONE, ordinary_b, E_FAIL, L"wrong launch type") &&
            first.Respond(RSP_DONE, ordinary_b);
        Check(responses && recorder.Wait(4), "reader consumes interleaved response frames within deadline");
        const auto events = recorder.Snapshot();
        Check(events.size() == 4 && events[0].id == launch_b && events[0].launch &&
            events[0].hr == static_cast<uint32_t>(E_ACCESSDENIED) && events[0].error == L"launch failure" &&
            events[1].id == ordinary_a && !events[1].launch &&
            events[2].id == launch_a && events[2].launch && events[2].hr == S_OK &&
            events[3].id == ordinary_b && !events[3].launch && events[3].hr == S_OK && Empty(client),
            "wrong types preserve pending requests; launch and ordinary callbacks stay distinct; duplicate fires once");
        const uint32_t stale = client.Launch(LaunchRequest());
        Check(first.ReadRequest(REQ_SHELL_LAUNCH, stale), "leave one dispatched launch pending at Stop");
        client.Stop();
        Check(Empty(client) && !client.reader_.joinable() && !client.CurrentConnection() &&
            recorder.Snapshot().size() == 5, "Stop cancels reader and completes pending launch exactly once");
        const bool second_connected = second.Connect();
        Check(second_connected, "connect a fresh fixture for restarted reader lifetime");
        if (!second_connected) return;
        endpoint = second.ReleaseClient();
        if (endpoint == INVALID_HANDLE_VALUE) return;
        Prepare(client, endpoint, recorder);
        const uint32_t fresh = client.Launch(LaunchRequest());
        const uint32_t barrier = client.CreateNewFile(L"restart-fixture-barrier");
        Check(second.ReadRequest(REQ_SHELL_LAUNCH, fresh) && second.ReadRequest(REQ_NEW_FILE, barrier),
            "restart assigns fresh request ids");
        Read(client);
        const bool restarted = second.Respond(RSP_SHELL_LAUNCH_DONE, stale) &&
            second.Respond(RSP_SHELL_LAUNCH_DONE, fresh) && second.Respond(RSP_DONE, barrier);
        Check(restarted && recorder.Wait(7), "new reader accepts fresh completions within deadline");
        client.Stop();
        const auto after = recorder.Snapshot();
        Check(fresh != stale && after.size() == 7 && after[5].id == fresh && after[5].launch &&
            after[6].id == barrier && !after[6].launch && Empty(client),
            "old completion cannot consume a new lifetime request or invoke a stale callback");
    }
};
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (argc == 2 && std::string(argv[1]) == "--exit-before-pipe") return 7;
    if (argc == 2 && std::string(argv[1]) == "--malformed-results") {
        pulse::ipc::ShellLaunchClientTestProbe::MalformedResults();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--multiwindow-owners") {
        pulse::ipc::ShellLaunchClientTestProbe::MultiwindowOwners();
        pulse::ipc::ShellLaunchClientTestProbe::MultiwindowOwnerFrames();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--startup-recovery") {
        pulse::ipc::ShellLaunchClientTestProbe::EarlyStartupExit();
        pulse::ipc::ShellLaunchClientTestProbe::FailureMessageSnapshot();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--transport-liveness") {
        pulse::ipc::ShellLaunchClientTestProbe::Backpressure();
        pulse::ipc::ShellLaunchClientTestProbe::FrameLiveness();
        pulse::ipc::ShellLaunchClientTestProbe::RetiredStream();
        return failures ? 1 : 0;
    }
    pulse::ipc::ShellLaunchClientTestProbe::EarlyStartupExit();
    pulse::ipc::ShellLaunchClientTestProbe::FailureMessageSnapshot();
    pulse::ipc::ShellLaunchClientTestProbe::FailedSubmission();
    pulse::ipc::ShellLaunchClientTestProbe::DisconnectPolicy();
    pulse::ipc::ShellLaunchClientTestProbe::TransportRouting();
    pulse::ipc::ShellLaunchClientTestProbe::InteractionReplyRouting();
    pulse::ipc::ShellLaunchClientTestProbe::MalformedResults();
    pulse::ipc::ShellLaunchClientTestProbe::MultiwindowOwners();
    pulse::ipc::ShellLaunchClientTestProbe::MultiwindowOwnerFrames();
    return failures ? 1 : 0;
}
