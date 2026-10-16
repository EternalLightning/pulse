#include "content_agent.h"
#include "content_search.h"
#include "content_search_protocol.h"
#include "content_instant_session.h"
#include "../common/current_user_security.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <memory>
#include <set>

namespace pulse::index {
namespace {

bool ReadAll(HANDLE pipe, void* output, DWORD size) {
    auto* bytes = static_cast<uint8_t*>(output);
    while (size) {
        DWORD read = 0;
        if (!ReadFile(pipe, bytes, size, &read, nullptr) || !read) return false;
        bytes += read;
        size -= read;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* input, DWORD size) {
    const auto* bytes = static_cast<const uint8_t*>(input);
    while (size) {
        DWORD written = 0;
        if (!WriteFile(pipe, bytes, size, &written, nullptr) || !written) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

bool SendBatch(HANDLE pipe, const ContentSearchProgress& progress,
               const std::vector<ContentHit>& hits) {
    ipc::PayloadWriter writer;
    writer.PutU64(progress.generation);
    writer.PutU64(progress.scanned_files);
    writer.PutU64(progress.scanned_bytes);
    uint32_t flags = progress.done ? 1u : 0u;
    if (progress.truncated) flags |= 2u;
    if (progress.live) flags |= 4u;
    if (progress.delta) flags |= 8u;
    writer.PutU32(flags);
    writer.PutU32(progress.error);
    writer.PutU32(static_cast<uint32_t>(progress.phase));
    writer.PutU64(progress.total_files);
    writer.PutString(progress.current_root);
    writer.PutU32(static_cast<uint32_t>(hits.size()));
    for (const auto& hit : hits) {
        writer.PutString(hit.path);
        writer.PutString(hit.name);
        writer.PutString(hit.snippet);
        writer.PutU64(hit.size);
        writer.PutU64(hit.modified);
        writer.PutU32(hit.line);
        writer.PutU32(hit.group | (hit.removed ? 0x80000000u : 0u));
    }
    writer.PutU64(progress.index_revision);
    writer.PutU32(static_cast<uint32_t>(hits.size()));
    for(const auto& hit:hits) writer.PutU64(hit.file_id);
    content::PutSubscriptionStatus(writer, progress);
    const auto header = content::Header(content::RSP_BATCH,
        static_cast<uint32_t>(writer.data().size()));
    return WriteAll(pipe, &header, sizeof(header)) &&
        (writer.data().empty() || WriteAll(pipe, writer.data().data(),
                                          static_cast<DWORD>(writer.data().size())));
}

} // namespace

int RunContentAgent(const std::wstring& token) {
    if (token.empty() || token.size() > 64 ||
        !std::all_of(token.begin(), token.end(), [](wchar_t c) {
            return iswalnum(c) || c == L'-' || c == L'_';
        })) return ERROR_INVALID_PARAMETER;
    CurrentUserSecurityAttributes security;
    if (!security) return ERROR_ACCESS_DENIED;
    const std::wstring pipe_name = content::PipeName(token);
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, security.get());
    if (pipe == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        const DWORD error = GetLastError();
        CloseHandle(pipe);
        return static_cast<int>(error);
    }
    ipc::MsgHeader header{};
    if (!ReadAll(pipe, &header, sizeof(header)) || header.magic != content::kMagic ||
        header.type != content::REQ_SEARCH || header.payload_size > content::kMaximumPayload) {
        CloseHandle(pipe);
        return ERROR_INVALID_DATA;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size && !ReadAll(pipe, payload.data(), header.payload_size)) {
        CloseHandle(pipe);
        return ERROR_BROKEN_PIPE;
    }
    ContentSearchRequest request;
    if (!content::ParseSearchRequest(payload, request)) {
        CloseHandle(pipe);
        return ERROR_INVALID_DATA;
    }
    std::atomic<bool> cancelled{false};
    const bool ok = RunContentSearch(request, cancelled,
        [pipe](const ContentSearchProgress& progress, std::vector<ContentHit> hits) {
            return SendBatch(pipe, progress, hits);
        });
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return ok ? 0 : ERROR_CANCELLED;
}

int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, bool read_only) {
    return RunPersistentContentAgent(token, parent_pid, read_only ? ContentAgentMode::Observer : ContentAgentMode::LegacyWriter);
}
int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, ContentAgentMode mode,
                              const std::wstring& database_path) {
    if (token.empty() || token.size() > 64 || !parent_pid ||
        !std::all_of(token.begin(), token.end(), [](wchar_t c) { return iswalnum(c) || c == L'-' || c == L'_'; })) return ERROR_INVALID_PARAMETER;
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (!parent) return static_cast<int>(GetLastError());
    CurrentUserSecurityAttributes security;
    if (!security) { CloseHandle(parent); return ERROR_ACCESS_DENIED; }
    ContentIndex index(database_path, mode);
    std::atomic<bool> stopping{false};
    HANDLE accept_thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &accept_thread, THREAD_TERMINATE, FALSE, 0);
    HANDLE monitor_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread monitor([&] {
        HANDLE events[]{parent, monitor_stop};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
            stopping = true;
            if (accept_thread) CancelSynchronousIo(accept_thread);
        }
    });
    struct Handler { std::thread thread; std::shared_ptr<std::atomic<bool>> done; };
    std::vector<Handler> handlers;
    std::mutex sessions_mu;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> sessions;
    std::set<uint64_t> cancelled_generations;
    const auto pipe_name = content::PipeName(token);
    while (!stopping.load()) {
        for (auto it = handlers.begin(); it != handlers.end();) {
            if (it->done->load()) { it->thread.join(); it = handlers.erase(it); } else ++it;
        }
        // Keep one instance available for controls when all 16 query slots are occupied.
        HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            17, 64 * 1024, 64 * 1024, 0, security.get());
        if (pipe == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                if (WaitForSingleObject(parent, 25) == WAIT_OBJECT_0) stopping = true;
                continue;
            }
            break;
        }
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); if (stopping) break; continue; }
        if (stopping) { CloseHandle(pipe); break; }
        auto done = std::make_shared<std::atomic<bool>>(false);
        handlers.push_back({std::thread([&, pipe, done] {
            ipc::MsgHeader header{};
            bool valid = ReadAll(pipe, &header, sizeof(header)) && header.magic == content::kMagic && header.payload_size <= content::kMaximumPayload;
            std::vector<uint8_t> payload(valid ? header.payload_size : 0);
            if (valid && !payload.empty()) valid = ReadAll(pipe, payload.data(), header.payload_size);
            if (valid && (header.type == content::REQ_SEARCH || header.type == content::REQ_SUBSCRIBE)) {
                ContentSearchRequest request;
                if (content::ParseSearchRequest(payload, request) && request.mode == ContentSearchMode::Content) {
                    auto cancel = std::make_shared<std::atomic<bool>>(false);
                    request.subscribe = header.type == content::REQ_SUBSCRIBE;
                    bool admitted = false;
                    {
                        std::lock_guard lock(sessions_mu);
                        if (sessions.size() < 16 && !sessions.contains(request.generation)) {
                            *cancel = cancelled_generations.contains(request.generation);
                            sessions[request.generation] = cancel; admitted = true;
                        }
                    }
                    if (!admitted) {
                        ContentSearchProgress progress; progress.generation = request.generation;
                        progress.done = true; progress.error = ERROR_BUSY;
                        SendBatch(pipe, progress, {});
                        FlushFileBuffers(pipe); DisconnectNamedPipe(pipe); CloseHandle(pipe); *done = true;
                        return;
                    }
                    do {
                        uint64_t revision = request.after_revision;
                        const auto send = [&](const auto& progress, auto hits) {
                            if (progress.done) revision = progress.index_revision;
                            return SendBatch(pipe, progress, hits);
                        };
                        if (mode == ContentAgentMode::Instant) {
                            ContentIndex query({}, ContentAgentMode::Instant);
                            RunInstantContentSession(query, request, *cancel, send);
                            break;
                        }
                        const bool ok = request.task_scan && !request.incremental && !request.after_revision
                            ? index.SearchTask(request, *cancel, send) : index.Search(request, *cancel, send);
                        if (!ok || !request.subscribe || cancel->load() || stopping.load()) break;
                        request.after_revision = revision;
                        request.incremental = true;
                        while (!cancel->load() && !stopping.load() && !index.WaitForRevision(revision, *cancel, 250)) {}
                    } while (!cancel->load() && !stopping.load());
                    { std::lock_guard lock(sessions_mu); sessions.erase(request.generation); }
                }
            } else if (valid) {
                ipc::PayloadReader reader(payload.data(), payload.size());
                DWORD error = 0;
                switch (header.type) {
                case content::REQ_CONFIG: { ContentIndexConfig config; if (!content::GetConfig(reader, config) || !index.Configure(config)) error = ERROR_INVALID_PARAMETER; break; }
                case content::REQ_CANCEL: { uint64_t generation = 0; if (reader.GetU64(generation)) { std::lock_guard lock(sessions_mu); cancelled_generations.insert(generation); if (cancelled_generations.size() > 256) cancelled_generations.erase(cancelled_generations.begin()); auto it = sessions.find(generation); if (it != sessions.end()) *it->second = true; } break; }
                case content::REQ_PAUSE: index.Pause(true); break;
                case content::REQ_RESUME: index.Pause(false); break;
                case content::REQ_REBUILD: index.Rebuild(); break;
                case content::REQ_SHUTDOWN: stopping = true; if (accept_thread) CancelSynchronousIo(accept_thread); break;
                case content::REQ_STATUS: break;
                default: error = ERROR_INVALID_PARAMETER; break;
                }
                auto status = index.Status(); if (error) status.error = error;
                ipc::PayloadWriter writer; content::PutStatus(writer, status); content::PutConfig(writer, index.Configuration());
                const auto response = content::Header(content::RSP_STATUS, static_cast<uint32_t>(writer.data().size()));
                WriteAll(pipe, &response, sizeof(response)); WriteAll(pipe, writer.data().data(), static_cast<DWORD>(writer.data().size()));
            }
            FlushFileBuffers(pipe); DisconnectNamedPipe(pipe); CloseHandle(pipe); *done = true;
        }), done});
    }
    stopping = true;
    { std::lock_guard lock(sessions_mu); for (auto& entry : sessions) *entry.second = true; }
    for (auto& handler : handlers) { CancelSynchronousIo(handler.thread.native_handle()); handler.thread.join(); }
    SetEvent(monitor_stop); monitor.join();
    CloseHandle(monitor_stop); CloseHandle(parent); if (accept_thread) CloseHandle(accept_thread);
    return 0;
}
} // namespace pulse::index
