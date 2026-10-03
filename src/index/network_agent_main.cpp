#include "network_agent_protocol.h"
#include "network_agent_host.h"
#include "network_index.h"
#include "../ipc/protocol.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <windows.h>
#include <sddl.h>

using namespace pulse::index;
using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

namespace {

struct Agent {
    NetworkIndex index;
    std::atomic<bool> running{true};
    std::timed_mutex search_mu;
    std::mutex lease_mu;
    transport::Identity identity;
    HANDLE stop = nullptr;
    bool fixture = false;
    std::atomic<int> clients{0};
    std::atomic<ULONGLONG> last_activity{0};
} g;

// Every live Pulse window polls the agent once per second, so a long quiet
// period means the Pulse instance(s) that used it are gone (e.g. it was
// replaced by an update). Exit instead of lingering as an orphan forever.
constexpr ULONGLONG kIdleExitMs = 10ull * 60ull * 1000ull;

bool WriteFrame(HANDLE pipe, uint32_t type, uint32_t id,
                const std::vector<uint8_t>& payload) {
    if (payload.size() > agent::kMaxPayload) return false;
    MsgHeader header = agent::MakeHeader(type, id, static_cast<uint32_t>(payload.size()));
    const auto deadline = GetTickCount64() + 2000;
    return transport::Transfer(pipe, &header, sizeof(header), true, deadline, g.stop) &&
        (payload.empty() || transport::Transfer(pipe, const_cast<uint8_t*>(payload.data()), static_cast<DWORD>(payload.size()), true, deadline, g.stop));
}

std::vector<uint8_t> SearchPayload(const SearchResult& result) {
    PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>((std::min)(result.total, static_cast<size_t>(UINT32_MAX))));
    writer.PutU32(static_cast<uint32_t>((std::min)(result.hits.size(), static_cast<size_t>(UINT32_MAX))));
    for (const auto& hit : result.hits) {
        writer.PutString(hit.path);
        writer.PutString(hit.name);
        writer.PutU32(hit.is_dir ? 1u : 0u);
        writer.PutU32(static_cast<uint32_t>(hit.size));
        writer.PutU32(static_cast<uint32_t>(hit.size >> 32));
        writer.PutU32(static_cast<uint32_t>(hit.mtime));
        writer.PutU32(static_cast<uint32_t>(hit.mtime >> 32));
    }
    return writer.data();
}

std::vector<uint8_t> RootsPayload() {
    PayloadWriter writer;
    const auto roots = g.index.Roots();
    writer.PutU32(static_cast<uint32_t>(roots.size()));
    for (const auto& root : roots) {
        writer.PutString(root.path);
        uint32_t flags = root.online ? 1u : 0u;
        if (root.building) flags |= 2u;
        if (root.watching) flags |= 4u;
        writer.PutU32(flags);
        writer.PutU32(root.progress);
        writer.PutU32(static_cast<uint32_t>(root.indexed_items));
        writer.PutU32(static_cast<uint32_t>(root.indexed_items >> 32));
        writer.PutString(root.state);
        writer.PutString(root.error);
    }
    return writer.data();
}

std::vector<uint8_t> ResultPayload(bool ok, const std::wstring& error) {
    PayloadWriter writer;
    writer.PutU32(ok ? 1u : 0u);
    writer.PutString(error);
    return writer.data();
}

void SearchAndReply(HANDLE pipe, uint32_t id, Query query) {
    // NetworkIndex has a single latest-result slot. Serialize only actual
    // searches, never the accept loop or idle client reads; use server IDs.
    std::unique_lock search_lock(g.search_mu, std::defer_lock);
    if (!search_lock.try_lock_for(std::chrono::milliseconds(500))) {
        WriteFrame(pipe, agent::RSP_RESULT, id, ResultPayload(false, L"网络索引查询过载，请稍后重试")); return;
    }
    static uint32_t next_id = 1;
    const auto internal_id = ++next_id;
    g.index.SearchAsync(query, internal_id);
    SearchResult result;
    for (int i = 0; i < 600 && g.running; ++i) {
        if (g.index.TakeResult(internal_id, result)) {
            WriteFrame(pipe, agent::RSP_SEARCH, id, SearchPayload(result));
            return;
        }
        Sleep(10);
    }
    WriteFrame(pipe, agent::RSP_RESULT, id, ResultPayload(false, L"网络索引查询超时"));
}

std::wstring TrackingOwner(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return {};
    HANDLE token = nullptr;
    const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token);
    transport::RevertOrFailFast();
    if (!opened) return {};
    DWORD bytes = 0, session = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    std::wstring owner;
    if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) &&
        GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &bytes)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            owner = sid; LocalFree(sid); owner += L"-" + std::to_wstring(session);
        }
    }
    CloseHandle(token); return owner;
}

void SetNetworkTrackingLease(HANDLE pipe, const std::wstring& owner, bool enabled) {
    std::lock_guard lease_lock(g.lease_mu);
    static std::unordered_map<std::wstring, uint64_t> leases;
    ULONG process = 0;
    if (!GetNamedPipeClientProcessId(pipe, &process)) return;
    const auto now = ChangeTracker::Now();
    const auto prefix = owner + L":";
    const auto key = prefix + std::to_wstring(process);
    leases[key] = enabled ? now + 90 : 0;
    std::erase_if(leases, [now](const auto& entry) { return entry.second < now; });
    bool active = false;
    for (const auto& [id, expiry] : leases) if (id.starts_with(prefix)) active = true;
    g.index.SetChangeLease(owner, active);
}

bool HandleChanges(HANDLE pipe, const MsgHeader& hdr, const std::vector<uint8_t>& payload) {
    const auto owner = TrackingOwner(pipe);
    if (owner.empty()) return false;
    PayloadReader r(payload.data(), payload.size()); PayloadWriter w;
    if (hdr.type == agent::REQ_CHANGE_LEASE) {
        uint32_t enabled = 0;
        if (!r.GetU32(enabled) || enabled > 1 || r.remaining()) return false;
        SetNetworkTrackingLease(pipe, owner, enabled != 0);
        return WriteFrame(pipe, agent::RSP_CHANGE_LEASE, hdr.request_id, w.data());
    }
    ChangeResponse response;
    if (hdr.type == agent::REQ_CHANGE_SUMMARIES) {
        uint64_t since = 0; uint32_t count = 0;
        if (!r.GetU64(since) || !r.GetU32(count) || count > 256) return false;
        std::vector<std::wstring> paths;
        for (uint32_t i = 0; i < count; ++i) {
            std::wstring path;
            if (!r.GetString(path) || path.empty() || path.size() > 32767) return false;
            paths.push_back(std::move(path));
        }
        if (r.remaining()) return false;
        response = g.index.Changes().Summaries(owner, paths, since);
        w.PutU32(static_cast<uint32_t>(response.state)); w.PutU32(static_cast<uint32_t>(response.summaries.size()));
        for (auto& summary : response.summaries) {
            const auto coverage = g.index.ChangeCoverage(summary.path);
            if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && summary.count)) summary.state = coverage;
            w.PutString(summary.path); w.PutU64(summary.last_change); w.PutU32(summary.count);
            w.PutU32(static_cast<uint32_t>(summary.state));
            for (auto count_kind : summary.counts) w.PutU32(count_kind);
            w.PutU32(summary.initial_count);
            w.PutU32(summary.has_deleted ? 1u : 0u); w.PutU32(summary.incomplete ? 1u : 0u);
        }
        return WriteFrame(pipe, agent::RSP_CHANGE_SUMMARIES, hdr.request_id, w.data());
    }
    std::wstring path; uint64_t since = 0, before = 0; uint32_t limit = 0, filter = 0;
    if (!r.GetString(path) || path.empty() || path.size() > 32767 || !r.GetU64(since) ||
        !r.GetU64(before) || !r.GetU32(limit) || !r.GetU32(filter) || limit == 0 || limit > 200 ||
        (filter != UINT32_MAX && filter > 5) || r.remaining()) return false;
    response = g.index.Changes().Details(owner, path, since, before, limit, filter);
    const auto coverage = g.index.ChangeCoverage(path);
    if (coverage != ChangeState::Gap && !(coverage == ChangeState::NotCovered && !response.records.empty())) response.state = coverage;
    w.PutU32(static_cast<uint32_t>(response.state)); w.PutU64(response.next_cursor);
    w.PutU32(static_cast<uint32_t>(response.records.size()));
    for (const auto& record : response.records) {
        w.PutU64(record.id); w.PutU64(record.time); w.PutU32(static_cast<uint32_t>(record.kind));
        w.PutU32(record.is_dir ? 1u : 0u); w.PutU32(static_cast<uint32_t>(record.source));
        w.PutString(record.path); w.PutString(record.old_path);
    }
    return WriteFrame(pipe, agent::RSP_CHANGE_DETAILS, hdr.request_id, w.data());
}

void ClientLoop(HANDLE pipe) {
    while (g.running) {
        MsgHeader header{};
        const auto deadline = GetTickCount64() + 2000;
        if (!transport::Transfer(pipe, &header, sizeof(header), false, deadline, g.stop) ||
            header.magic != agent::kMagic || header.payload_size > 256 * 1024) break;
        std::vector<uint8_t> payload(header.payload_size);
        if (!payload.empty() && !transport::Transfer(pipe, payload.data(), header.payload_size, false, deadline, g.stop)) break;
        transport::Handle caller(transport::CaptureCaller(pipe));
        if (!caller.get() || !g.identity.Matches(transport::Identity::FromToken(caller.get()))) break;
        PayloadReader reader(payload.data(), payload.size());
        if (g.fixture && header.type == 0x7fff0001 && payload.empty()) {
            g.running = false; SetEvent(g.stop); break;
        }
        if (header.type >= agent::REQ_CHANGE_LEASE && header.type <= agent::REQ_CHANGE_DETAILS) {
            if (!HandleChanges(pipe, header, payload)) break;
        } else if (header.type == agent::REQ_ROOTS) {
            WriteFrame(pipe, agent::RSP_ROOTS, header.request_id, RootsPayload());
        } else if (header.type == agent::REQ_STATUS) {
            PayloadWriter writer;
            const auto roots = g.index.Roots();
            writer.PutU32(1u);
            uint64_t count = 0;
            for (const auto& root : roots) count += root.indexed_items;
            writer.PutU32(static_cast<uint32_t>((std::min)(count, static_cast<uint64_t>(UINT32_MAX))));
            writer.PutString(roots.empty() ? L"网络索引未配置" : L"网络索引代理运行中");
            WriteFrame(pipe, agent::RSP_STATUS, header.request_id, writer.data());
        } else if (header.type == agent::REQ_SEARCH) {
            Query query;
            uint32_t flags = 0, sort = 0, limit = 0, offset = 0;
            if (!reader.GetU32(flags) || !reader.GetU32(sort) || !reader.GetU32(limit) ||
                !reader.GetU32(offset) || !reader.GetString(query.needle) ||
                !reader.GetString(query.path_prefix) || reader.remaining() || flags > 7 ||
                sort > static_cast<uint32_t>(ResultSort::Mtime) || query.needle.size() > 4096 ||
                query.path_prefix.size() > 32768 || offset > 1000000) break;
            query.rank = (flags & 1u) != 0;
            query.folders_only = (flags & 2u) != 0;
            query.sort_desc = (flags & 4u) != 0;
            query.sort = static_cast<ResultSort>(sort);
            query.limit = (std::min)(static_cast<size_t>(limit), kSearchPageCap);
            query.offset = offset;
            SearchAndReply(pipe, header.request_id, std::move(query));
        } else if (header.type == agent::REQ_ADD_ROOT || header.type == agent::REQ_REMOVE_ROOT) {
            std::wstring root, error;
            if (!reader.GetString(root) || reader.remaining() || root.size() > 32768) break;
            const bool ok = header.type == agent::REQ_ADD_ROOT
                ? g.index.AddRoot(root, &error) : g.index.RemoveRoot(root, &error);
            WriteFrame(pipe, agent::RSP_RESULT, header.request_id, ResultPayload(ok, error));
        } else if (header.type == agent::REQ_REBUILD) {
            std::wstring root;
            if (!reader.GetString(root)) root.clear();
            g.index.Rebuild(root);
            WriteFrame(pipe, agent::RSP_RESULT, header.request_id, ResultPayload(true, {}));
        }
    }
    CloseHandle(pipe);
}

int RunAgent(const std::wstring& fixture_tag = {}) {
    g.fixture = !fixture_tag.empty();
    g.identity = transport::Identity::Current();
    transport::LogonSecurity security(g.identity);
    const auto name = g.fixture ? L"\\\\.\\pipe\\PulseNetworkIndex.Test." + fixture_tag : agent::PipeName();
    const auto mutex_name = g.fixture ? L"Local\\Pulse.Index.NetworkAgent.Test." + fixture_tag : agent::SingletonName();
    if (!security.get() || name.empty() || mutex_name.empty()) return ERROR_ACCESS_DENIED;
    HANDLE singleton = CreateMutexW(security.get(), TRUE, mutex_name.c_str());
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    g.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g.stop) { CloseHandle(singleton); return ERROR_NOT_ENOUGH_MEMORY; }
    if (!g.fixture) g.index.Start(nullptr, 0, 0);
    struct Worker { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Worker> workers;
    bool first = true;
    g.last_activity = GetTickCount64();
    std::thread idle_watch([] {
        while (g.running) {
            Sleep(1000);
            if (!g.running || g.clients.load() != 0 ||
                GetTickCount64() - g.last_activity.load() < kIdleExitMs) continue;
            g.running = false;
            SetEvent(g.stop);
        }
    });
    int exit_code = 0;
    while (g.running) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (!it->done->load()) { ++it; continue; }
            it->thread.join(); it = workers.erase(it);
        }
        const DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(name.c_str(), flags,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            16, 64 * 1024, 64 * 1024, 0, security.get());
        if (pipe == INVALID_HANDLE_VALUE) {
            if (first) { exit_code = static_cast<int>(GetLastError()); break; }
            WaitForSingleObject(g.stop, 50); continue;
        }
        first = false;
        transport::Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED operation{}; operation.hEvent = event.get();
        bool connected = event.get() && ConnectNamedPipe(pipe, &operation);
        if (!connected && event.get()) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) {
                HANDLE waits[]{event.get(), g.stop}; DWORD done = 0;
                if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0)
                    connected = GetOverlappedResult(pipe, &operation, &done, FALSE) != FALSE;
                else { CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &done, TRUE); }
            }
        }
        if (!connected || !g.running || g.clients >= 15) { CloseHandle(pipe); continue; }
        ++g.clients; g.last_activity = GetTickCount64();
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.push_back({std::thread([pipe, done] {
            ClientLoop(pipe); g.last_activity = GetTickCount64(); --g.clients; *done = true;
        }), done});
    }
    g.running = false;
    SetEvent(g.stop);
    if (idle_watch.joinable()) idle_watch.join();
    for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
    g.index.Stop();
    CloseHandle(g.stop); g.stop = nullptr;
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    return exit_code;
}

} // namespace

int pulse::index::RunNetworkAgent() {
    return RunAgent();
}
int pulse::index::RunNetworkAgentFixture(const std::wstring& tag) {
    if (tag.empty() || tag.size() > 64 || tag.find_first_not_of(L"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::wstring::npos)
        return ERROR_INVALID_PARAMETER;
    return RunAgent(tag);
}
