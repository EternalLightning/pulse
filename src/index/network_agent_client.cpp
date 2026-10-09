#include "network_agent_client.h"
#include "index_search_response.h"
#include "live_filename_search.h"
#include "../ipc/protocol.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <shellapi.h>

namespace pulse::index {
namespace {

using pulse::ipc::MsgHeader;
using pulse::ipc::PayloadReader;
using pulse::ipc::PayloadWriter;
using pulse::ipc::PipeRead;
using pulse::ipc::PipeWrite;

std::vector<uint8_t> QueryPayload(const Query& query) {
    PayloadWriter writer;
    uint32_t flags = 0;
    if (query.rank) flags |= 1u;
    if (query.folders_only) flags |= 2u;
    if (query.sort_desc) flags |= 4u;
    writer.PutU32(flags);
    writer.PutU32(static_cast<uint32_t>(query.sort));
    writer.PutU32(static_cast<uint32_t>(query.limit));
    writer.PutU32(static_cast<uint32_t>(query.offset));
    writer.PutString(query.needle);
    writer.PutString(query.path_prefix);
    return writer.data();
}

bool ReadFrame(HANDLE pipe, uint32_t& type, uint32_t& id, std::vector<uint8_t>& payload,
               ULONGLONG deadline, HANDLE cancel) {
    MsgHeader header{};
    if (!transport::Transfer(pipe, &header, sizeof(header), false, deadline, cancel) ||
        header.magic != agent::kMagic || header.payload_size > agent::kMaxPayload) return false;
    payload.resize(header.payload_size);
    if (!payload.empty() && !transport::Transfer(pipe, payload.data(), header.payload_size, false, deadline, cancel)) return false;
    type = header.type;
    id = header.request_id;
    return true;
}

} // namespace

std::wstring NetworkAgentClient::ExePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return L"Pulse.Index.exe";
    return std::wstring(path, slash + 1) + L"Pulse.Index.exe";
}

bool NetworkAgentClient::EnsureAgent() {
    if (agent_process_ && WaitForSingleObject(agent_process_, 0) == WAIT_TIMEOUT) return true;
    if (agent_process_) {
        CloseHandle(agent_process_);
        agent_process_ = nullptr;
    }
    // The agent is a per-session singleton (see RunAgent in network_agent_main.cpp). It may be
    // owned by another Pulse window or outlive the Pulse instance that started it. Spawning a
    // duplicate would exit immediately and cause a respawn loop on every request (and a
    // flickering AppStarting cursor), so reuse whichever agent currently holds the singleton.
    const auto singleton = agent::SingletonName();
    if (singleton.empty()) return false;
    if (HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, singleton.c_str())) {
        CloseHandle(existing);
        return true;
    }
    if (GetLastError() == ERROR_ACCESS_DENIED) return false; // fail closed on a squatted endpoint
    // A crashing agent must not be relaunched on every 1 s status poll.
    const ULONGLONG now = GetTickCount64();
    if (last_spawn_tick_ && now - last_spawn_tick_ < kRespawnBackoffMs) return false;
    const std::wstring exe = ExePath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe + L"\" --network-agent";
    last_spawn_tick_ = now;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    agent_process_ = process.hProcess;
    return true;
}

bool NetworkAgentClient::OpenPipe(HANDLE& pipe) {
    const auto name = agent::PipeName();
    if (name.empty()) return false;
    const auto deadline = GetTickCount64() + 2000;
    do {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (transport::VerifyServer(pipe, ExePath(), transport::Identity::Current())) return true;
            CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; return false;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 100);
        else Sleep(20);
    } while (running_ && GetTickCount64() < deadline);
    return false;
}

bool NetworkAgentClient::Request(uint32_t type, uint32_t id,
                                 const std::vector<uint8_t>& payload,
                                 uint32_t& response_type,
                                 std::vector<uint8_t>& response) {
    std::lock_guard<std::mutex> request_lock(request_mu_);
    if (!running_ || !EnsureAgent()) return false;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    if (!OpenPipe(pipe)) return false;
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (!running_) {
            CloseHandle(pipe);
            return false;
        }
        active_pipe_ = pipe;
    }
    const auto close_pipe = [this, pipe](void*) {
        const DWORD error = GetLastError();
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (active_pipe_ != pipe) return;
        active_pipe_ = INVALID_HANDLE_VALUE;
        CloseHandle(pipe);
        SetLastError(error);
    };
    const std::unique_ptr<void, decltype(close_pipe)> pipe_owner(pipe, close_pipe);
    const MsgHeader header = agent::MakeHeader(type, id, static_cast<uint32_t>(payload.size()));
    const auto deadline = GetTickCount64() + 10000;
    const bool sent = transport::Transfer(pipe, const_cast<MsgHeader*>(&header), sizeof(header), true, deadline, cancel_) &&
        (payload.empty() || transport::Transfer(pipe, const_cast<uint8_t*>(payload.data()), static_cast<DWORD>(payload.size()), true, deadline, cancel_));
    if (!sent) {
        return false;
    }
    uint32_t response_id = 0;
    const bool received = ReadFrame(pipe, response_type, response_id, response, deadline, cancel_) && response_id == id;
    if (!received && GetLastError() == ERROR_SUCCESS) SetLastError(ERROR_INVALID_DATA);
    return received;
}

void NetworkAgentClient::Start(HWND notify, UINT status_msg, UINT search_msg) {
    Stop();
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel_) return;
    running_ = true;
    status_thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(status_mu_);
        while (running_) {
            lock.unlock();
            RefreshRoots();
            lock.lock();
            if (status_cv_.wait_for(lock, std::chrono::seconds(1),
                                    [this] { return !running_; })) break;
        }
    });
    search_thread_ = std::thread([this] { SearchLoop(); });
}

void NetworkAgentClient::Stop() {
    running_ = false;
    {
        std::lock_guard lock(search_mu_);
        for (const auto& live : live_searches_) {
            live->cancelled = true;
            if (live->worker.joinable()) CancelSynchronousIo(live->worker.native_handle());
        }
    }
    status_cv_.notify_all();
    search_cv_.notify_all();
    if (cancel_) SetEvent(cancel_);
    {
        std::lock_guard<std::mutex> lock(pipe_mu_);
        if (active_pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(active_pipe_, nullptr);
    }
    if (status_thread_.joinable()) status_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    for (const auto& live : live_searches_) if (live->worker.joinable()) live->worker.join();
    live_searches_.clear();
    std::lock_guard<std::mutex> lock(request_mu_);
    if (cancel_) { CloseHandle(cancel_); cancel_ = nullptr; }
    if (agent_process_) {
        CloseHandle(agent_process_);
        agent_process_ = nullptr;
    }
}

void NetworkAgentClient::SearchAsync(const Query& query, uint32_t id, HWND reply, UINT reply_message) {
    if (!running_) return;
    const bool live_search = UsesLiveSearch(query.path_prefix);
    latest_search_id_.store(id);
    {
        std::lock_guard<std::mutex> lock(search_mu_);
        for (auto it = live_searches_.begin(); it != live_searches_.end();) {
            if ((*it)->session == query.session_id) {
                (*it)->cancelled = true;
                CancelSynchronousIo((*it)->worker.native_handle());
            }
            if ((*it)->done) {
                (*it)->worker.join();
                it = live_searches_.erase(it);
            } else ++it;
        }
        pending_query_ = query;
        pending_search_id_ = id;
        std::lock_guard result_lock(mu_);
        if (auto previous = session_requests_.find(query.session_id); previous != session_requests_.end()) {
            results_.erase(previous->second);
            replies_.erase(previous->second);
        }
        replies_[id] = {reply ? reply : notify_, reply ? reply_message : search_msg_};
        session_requests_[query.session_id]=id;
        if (live_search) {
            pending_searches_.erase(query.session_id);
            auto live = std::make_unique<LiveSearch>();
            live->session = query.session_id;
            live_searches_.reserve(live_searches_.size() + 1);
            auto* state = live.get();
            live->worker = std::thread([this, state, query, id] {
                const auto publish = [this, state, query, id](SearchResult result) {
                    std::lock_guard lock(search_mu_);
                    const auto current = session_requests_.find(query.session_id);
                    if (!state->cancelled && current != session_requests_.end() && current->second == id)
                        PublishResult(id, std::move(result));
                };
                try { ScanLiveFilenames(query, FilenameSearchRoot(query.path_prefix), state->cancelled, publish); }
                catch (...) { publish(SearchFailure(ERROR_NOT_ENOUGH_MEMORY)); }
                state->done = true;
            });
            live_searches_.push_back(std::move(live));
            have_pending_search_ = !pending_searches_.empty();
            return;
        }
        pending_searches_[query.session_id]={id,query};
        have_pending_search_ = true;
    }
    search_cv_.notify_one();
}

void NetworkAgentClient::SearchLoop() {
    std::unique_lock<std::mutex> lock(search_mu_);
    while (running_) {
        search_cv_.wait(lock, [this] { return !running_ || have_pending_search_; });
        if (!running_) break;
        auto next=pending_searches_.begin();
        if(next==pending_searches_.end()) {have_pending_search_=false;continue;}
        Query query = std::move(next->second.second);
        const uint32_t id = next->second.first;
        pending_searches_.erase(next);have_pending_search_=!pending_searches_.empty();
        lock.unlock();
        SearchRequest(std::move(query), id);
        lock.lock();
    }
}

void NetworkAgentClient::SearchRequest(Query query, uint32_t id) {
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    try {
        SetLastError(ERROR_SUCCESS);
        const bool received = Request(agent::REQ_SEARCH, id, QueryPayload(query), response_type, payload);
        const DWORD error = received ? ERROR_SUCCESS : GetLastError();
        HandleSearchResponse(query, id, received, response_type, payload, error);
    } catch (const std::bad_alloc&) {
        HandleSearchResponse(query, id, false, 0, {}, ERROR_NOT_ENOUGH_MEMORY);
    } catch (const std::length_error&) {
        HandleSearchResponse(query, id, false, 0, {}, ERROR_INVALID_DATA);
    }
}

void NetworkAgentClient::HandleSearchResponse(const Query& query, uint32_t id, bool received,
                                             uint32_t response_type, const std::vector<uint8_t>& payload,
                                             DWORD error) {
    SearchResult result;
    if (!received) result = SearchFailure(error);
    else if (response_type == agent::RSP_SEARCH) {
        if (!DecodeSearchResponse(payload.data(), payload.size(), result))
            result = SearchFailure(result.error ? result.error : ERROR_INVALID_DATA);
    } else if (response_type == agent::RSP_RESULT) {
        // Compatibility with older network helpers, which returned only text.
        PayloadReader reader(payload.data(), payload.size());
        uint32_t ok = 0;
        std::wstring text;
        const bool valid = reader.GetU32(ok) && ok == 0 && reader.GetString(text) && !reader.remaining();
        result = SearchFailure(valid ? ERROR_GEN_FAILURE : ERROR_INVALID_DATA);
    } else result = SearchFailure(ERROR_INVALID_DATA);
    std::lock_guard lock(search_mu_);
    const auto current = session_requests_.find(query.session_id);
    if (current == session_requests_.end() || current->second != id) return;
    PublishResult(id, std::move(result));
}

void NetworkAgentClient::PublishResult(uint32_t id, SearchResult result) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto reply = replies_.find(id);
    if (reply == replies_.end()) return;
    results_[id] = std::move(result);
    if (reply->second.window && reply->second.message)
        PostMessageW(reply->second.window, reply->second.message, id, 0);
}

void NetworkAgentClient::CancelSession(uint64_t session_id) {
    std::lock_guard search_lock(search_mu_);
    for (const auto& live : live_searches_) if (live->session == session_id) {
        live->cancelled = true;
        CancelSynchronousIo(live->worker.native_handle());
    }
    std::lock_guard result_lock(mu_);
    if (auto current = session_requests_.find(session_id); current != session_requests_.end()) {
        results_.erase(current->second);
        replies_.erase(current->second);
        session_requests_.erase(current);
    }
    pending_searches_.erase(session_id);
    have_pending_search_ = !pending_searches_.empty();
}

bool NetworkAgentClient::TakeResult(uint32_t id, SearchResult& result, HWND reply) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto route = replies_.find(id);
    if (route == replies_.end() || route->second.window != (reply ? reply : notify_)) return false;
    auto found=results_.find(id);if(found==results_.end()) return false;
    result=std::move(found->second);results_.erase(found);
    return true;
}

void NetworkAgentClient::RefreshRoots() {
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_ROOTS, 1, {}, response_type, payload) || response_type != agent::RSP_ROOTS)
        return;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t count = 0;
    if (!reader.GetU32(count) || count > 1024 || count > reader.remaining() / (3 * sizeof(uint32_t) + 4 * sizeof(uint32_t))) return;
    std::vector<NetworkRootInfo> roots;
    roots.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        NetworkRootInfo root;
        uint32_t flags = 0, progress = 0, lo = 0, hi = 0;
        if (!reader.GetString(root.path) || !reader.GetU32(flags) || !reader.GetU32(progress) ||
            !reader.GetU32(lo) || !reader.GetU32(hi) || !reader.GetString(root.state) ||
            !reader.GetString(root.error)) return;
        root.online = (flags & 1u) != 0;
        root.building = (flags & 2u) != 0;
        root.watching = (flags & 4u) != 0;
        root.progress = progress;
        root.indexed_items = (static_cast<uint64_t>(hi) << 32) | lo;
        roots.push_back(std::move(root));
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        roots_ = std::move(roots);
    }
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

std::vector<NetworkRootInfo> NetworkAgentClient::Roots() const {
    std::lock_guard<std::mutex> lock(mu_);
    return roots_;
}

bool NetworkAgentClient::UsesLiveSearch(const std::wstring& path) const {
    const auto root = FilenameSearchRoot(path);
    if (root.empty()) return false;
    std::lock_guard lock(mu_);
    for (const auto& configured : roots_) {
        const auto indexed = FilenameSearchRoot(configured.path);
        if (!indexed.empty() && FilenameSearchCovered(root, indexed)) return false;
    }
    return true;
}

bool NetworkAgentClient::AddRoot(const std::wstring& path, std::wstring* error) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_ADD_ROOT, 2, writer.data(), response_type, payload) ||
        response_type != agent::RSP_RESULT) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t ok = 0;
    std::wstring message;
    if (!reader.GetU32(ok) || !reader.GetString(message)) return false;
    if (!ok && error) *error = std::move(message);
    RefreshRoots();
    return ok != 0;
}

bool NetworkAgentClient::RemoveRoot(const std::wstring& path, std::wstring* error) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> payload;
    if (!Request(agent::REQ_REMOVE_ROOT, 3, writer.data(), response_type, payload) ||
        response_type != agent::RSP_RESULT) return false;
    PayloadReader reader(payload.data(), payload.size());
    uint32_t ok = 0;
    std::wstring message;
    if (!reader.GetU32(ok) || !reader.GetString(message)) return false;
    if (!ok && error) *error = std::move(message);
    RefreshRoots();
    return ok != 0;
}

void NetworkAgentClient::Rebuild(const std::wstring& path) {
    PayloadWriter writer;
    writer.PutString(path);
    uint32_t response_type = 0;
    std::vector<uint8_t> ignored;
    Request(agent::REQ_REBUILD, 4, writer.data(), response_type, ignored);
    RefreshRoots();
}

} // namespace pulse::index
