// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../index/index_search_response.h"
#include "../index/live_filename_search.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace pulse::index {
struct IndexClientTestAccess {
    static void Initialize(IndexClient& client, HWND window, UINT message) {
        client.notify_ = window; client.search_msg_ = message; client.running_ = true;
    }
    static void Publish(IndexClient& client, uint32_t id) {
        ipc::PayloadWriter payload;
        payload.PutU32(1); payload.PutU32(1);
        payload.PutString(L"C:\\fixture\\local.txt"); payload.PutString(L"local.txt");
        for (int i = 0; i < 5; ++i) payload.PutU32(0);
        client.HandleSearch(id, payload.data().data(), payload.data().size());
    }
    static void Respond(IndexClient& client, uint32_t id, const std::vector<uint8_t>& payload) {
        client.HandleSearch(id, payload.data(), payload.size());
    }
    static void Volumes(IndexClient& client, const std::vector<uint8_t>& payload) {
        client.HandleVolumes(payload.data(), payload.size());
    }
    static void StartWriter(IndexClient& client, int mode) {
        client.volume_refresh_requested_ = mode == 0 || mode == 4;
        if (mode == 2) {
            Query query; query.session_id = 1;
            client.pending_searches_[1] = {17, query};
            client.have_pending_ = true;
        }
        if (mode == 3) client.cancelled_sessions_.push_back(1);
        client.connected_ = mode == 4;
        client.running_ = true;
        client.writer_ = std::thread([&client] { client.Writer(); });
    }
    static uint64_t WriterCpu(IndexClient& client) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(client.writer_.native_handle(), &created, &exited, &kernel, &user)) return UINT64_MAX;
        return (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
            (static_cast<uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
    }
    static bool Pending(IndexClient& client, int mode) {
        std::lock_guard lock(client.mu_);
        if (mode == 2) return client.have_pending_ && client.pending_searches_.contains(1);
        if (mode == 3) return client.cancelled_sessions_.size() == 1;
        return mode == 1 || client.volume_refresh_requested_;
    }
    static void ConnectWriter(IndexClient& client, HANDLE pipe) {
        { std::lock_guard lock(client.pipe_mu_); client.connection_ = std::make_shared<IndexClient::Connection>(pipe); }
        {
            std::lock_guard lock(client.mu_);
            client.connected_ = true;
        }
        client.pending_cv_.notify_one();
    }
    static void AttachIo(IndexClient& client, HANDLE pipe) {
        std::lock_guard lock(client.pipe_mu_);
        client.connection_ = std::make_shared<IndexClient::Connection>(pipe);
        client.running_ = true;
        client.connected_ = true;
    }
    static void ReadIo(IndexClient& client, HANDLE done, bool& received, DWORD& error) {
        client.worker_ = std::thread([&client, done, &received, &error] {
            ipc::MsgHeader header{}; std::vector<uint8_t> payload;
            received = client.ReadMsg(header, payload);
            error = received ? ERROR_SUCCESS : GetLastError();
            SetEvent(done);
        });
    }
    static void BodyGate(IndexClient& client, std::function<void()> gate) {
        client.before_frame_body_ = std::move(gate);
    }
    static bool Running(IndexClient& client) { return client.running_.load(); }
    static void WriteIo(IndexClient& client, HANDLE done, bool& sent) {
        client.writer_ = std::thread([&client, done, &sent] {
            const std::vector<uint8_t> payload(1024 * 1024, 0x53);
            sent = client.WriteMsg(REQ_IDX_SEARCH, 1, payload);
            SetEvent(done);
        });
    }
    static void DisconnectIo(IndexClient& client, HANDLE done) {
        client.worker_ = std::thread([&client, done] { client.Disconnect(); SetEvent(done); });
    }
};
struct SearchClientRoutingTestPeer {
    static void SetRoots(NetworkAgentClient& client, const std::vector<NetworkRootInfo>& roots) {
        std::lock_guard lock(client.mu_); client.roots_ = roots;
    }
    static void Initialize(NetworkAgentClient& client, HWND window, UINT message) {
        client.notify_ = window; client.search_msg_ = message; client.running_ = true;
    }
    static void Publish(NetworkAgentClient& client, uint32_t id) {
        SearchResult result; result.total = 1;
        client.PublishResult(id, std::move(result));
    }
    static void Respond(NetworkAgentClient& client, const Query& query, uint32_t id,
                        bool received, uint32_t type, const std::vector<uint8_t>& payload, DWORD error = 0) {
        client.HandleSearchResponse(query, id, received, type, payload, error);
    }
};
}

#include "index_client_io_cases.h"

namespace {
constexpr UINT kMain = WM_APP + 201, kPopup = WM_APP + 202;
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
bool Posted(HWND window, UINT message, uint32_t id) {
    MSG msg{};
    return PeekMessageW(&msg, window, message, message, PM_REMOVE) && msg.wParam == id;
}

void WriterIdle() {
    using pulse::index::IndexClient;
    using Access = pulse::index::IndexClientTestAccess;
    const char* labels[]{"disconnected volume request", "disconnected no-work control",
        "disconnected pending search", "disconnected pending cancellation", "connected failed write"};
    for (int mode = 0; mode < 5; ++mode) {
        IndexClient client;
        Access::StartWriter(client, mode);
        const auto before = Access::WriterCpu(client);
        const auto started = GetTickCount64();
        Sleep(500);
        const auto after = Access::WriterCpu(client);
        const auto elapsed = GetTickCount64() - started;
        const double percent = before != UINT64_MAX && after != UINT64_MAX && after >= before
            ? static_cast<double>(after - before) / (elapsed * 100.0) : 100.0;
        std::printf("[INFO] %s single_core_cpu_percent=%.3f\n", labels[mode], percent);
        Check(percent < 10.0, labels[mode]);
        const auto stopping = GetTickCount64();
        client.Stop();
        Check(GetTickCount64() - stopping < 500, "writer stop wakes the idle or retry wait");
        Check(Access::Pending(client, mode), "waiting or failed transport retains pending work");
    }
    const auto pipe_name = L"\\\\.\\pipe\\pulse-writer-fixture-" + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 0, nullptr);
    HANDLE sender = server == INVALID_HANDLE_VALUE ? INVALID_HANDLE_VALUE :
        CreateFileW(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    Check(server != INVALID_HANDLE_VALUE && sender != INVALID_HANDLE_VALUE, "isolated reconnect transport created");
    if (sender != INVALID_HANDLE_VALUE) {
        IndexClient client;
        Access::StartWriter(client, 0);
        pulse::index::Query query; query.session_id = 1; query.needle = L"fixture";
        client.SearchAsync(query, 17);
        client.CancelSession(2);
        Access::ConnectWriter(client, sender);
        pulse::ipc::PipeIoOptions options; options.pending_timeout_ms = 1500;
        for (const auto expected : {8u, static_cast<unsigned>(pulse::index::REQ_IDX_VOLUMES),
                                   static_cast<unsigned>(pulse::index::REQ_IDX_SEARCH)}) {
            pulse::ipc::MsgHeader header{};
            bool received = pulse::ipc::PipeRead(server, &header, sizeof(header), options);
            std::vector<uint8_t> payload(received && header.payload_size <= 4096 ? header.payload_size : 0);
            received = received && header.payload_size <= 4096 &&
                pulse::ipc::PipeRead(server, payload.data(), static_cast<DWORD>(payload.size()), options);
            Check(received && header.magic == pulse::index::kIndexMagic && header.type == expected,
                "reconnection wakes writer and delivers retained cancellation, scope and search");
        }
        client.Stop();
    }
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
}

std::vector<uint8_t> Response(uint64_t revision, DWORD error, bool incomplete) {
    pulse::ipc::PayloadWriter payload;
    payload.PutU32(0); payload.PutU32(0);
    payload.PutU64(revision); payload.PutU32(error); payload.PutU32(incomplete ? 1u : 0u);
    return payload.data();
}

void LiveSearch(HWND window) {
    using namespace pulse::index;
    NetworkAgentClient client;
    Check(client.UsesLiveSearch(L"\\\\server\\share\\folder"), "unindexed UNC folder uses background filename enumeration");
    Check(client.UsesLiveSearch(L"\\\\?\\UNC\\server\\share\\folder"), "extended UNC folder uses background filename enumeration");
    Check(!client.UsesLiveSearch(L"C:\\fixture") && !client.UsesLiveSearch(L"\\\\server") &&
          !client.UsesLiveSearch(L"\\\\?\\C:\\fixture"), "local, server-only and device paths do not trigger network scan");
    NetworkRootInfo root; root.path = L"\\\\SERVER\\Share";
    SearchClientRoutingTestPeer::SetRoots(client, {root});
    Check(!client.UsesLiveSearch(L"\\\\server\\share\\folder") &&
          client.UsesLiveSearch(L"\\\\server\\share-sibling"), "coverage is case-insensitive and requires a directory boundary");
    root.path = L"\\\\server\\share\\child";
    SearchClientRoutingTestPeer::SetRoots(client, {root});
    Check(client.UsesLiveSearch(L"\\\\server\\share"), "an indexed descendant does not hide unindexed siblings");

    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"unc-live-" + std::to_wstring(GetCurrentProcessId())));
    const auto extended = std::filesystem::path(L"\\\\?\\" + directory.wstring());
    std::filesystem::create_directories(extended / L"nested");
    std::ofstream(extended / L"match-root.txt") << "root";
    std::ofstream(extended / L"nested" / L"MATCH-child.txt") << "child";
    std::ofstream(extended / L"nested" / L"ignored.log") << "ignored";
    auto deep = extended;
    for (int i = 0; i < 6; ++i) deep /= L"long-folder-name-for-background-network-filename-scan";
    std::filesystem::create_directories(deep);
    std::ofstream(deep / L"match-long.txt") << "long";
    const bool junction = CreateSymbolicLinkW((extended / L"cycle").c_str(), extended.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
    Query query; query.needle = L"match ext:txt"; query.rank = false; query.sort = ResultSort::Name;
    query.limit = 1;
    std::atomic<bool> cancelled{false};
    std::vector<SearchResult> updates;
    ScanLiveFilenames(query, directory.wstring(), cancelled, [&](SearchResult result) { updates.push_back(std::move(result)); });
    Check(updates.size() >= 2 && updates.front().scanning && !updates.back().scanning &&
          updates.back().total == 3 && updates.back().hits.size() == 1 && !updates.back().error,
          "recursive scan publishes progress and final filtered bounded page including long paths");
    Check(updates.back().hits.front().name == L"MATCH-child.txt", "scan retains globally sorted page candidates");
    query.offset = 1;
    updates.clear();
    ScanLiveFilenames(query, directory.wstring(), cancelled, [&](SearchResult result) { updates.push_back(std::move(result)); });
    Check(updates.back().hits.size() == 1 && updates.back().hits.front().name == L"match-long.txt" &&
          updates.back().total == 3, "load-more offset is applied after recursive global sorting");
    if (junction) Check(updates.back().total == 3, "directory reparse cycle is not recursively followed");
    query.offset = 0; query.needle = L"no-such-match";
    updates.clear();
    ScanLiveFilenames(query, directory.wstring(), cancelled, [&](SearchResult result) { updates.push_back(std::move(result)); });
    Check(!updates.back().scanning && !updates.back().total && !updates.back().error, "empty scan has a successful final update");
    updates.clear();
    ScanLiveFilenames(query, directory.wstring(), cancelled, [&](SearchResult result) {
        updates.push_back(std::move(result)); cancelled = true;
    });
    Check(updates.size() == 1 && updates.front().scanning, "cancelled scan does not publish a late final update");
    cancelled = false; updates.clear();
    ScanLiveFilenames(query, (directory / L"missing").wstring(), cancelled,
        [&](SearchResult result) { updates.push_back(std::move(result)); });
    Check(!updates.back().scanning && updates.back().incomplete && updates.back().error,
          "inaccessible root publishes an explicit incomplete terminal");
    std::filesystem::remove_all(extended);
    // Exercise stale/cancelled publications through the same session routes.
    SearchClientRoutingTestPeer::Initialize(client, window, kMain);
    Query first; first.session_id = 99;
    client.SearchAsync(first, 901); client.SearchAsync(first, 902);
    SearchClientRoutingTestPeer::Publish(client, 901);
    Check(!Posted(window, kMain, 901), "replaced live query rejects stale generation route");
    client.CancelSession(first.session_id);
    SearchClientRoutingTestPeer::Publish(client, 902);
    Check(!Posted(window, kMain, 902), "cancelled live query rejects late route publication");
    client.Stop();
}

void Boundaries(HWND window) {
    using namespace pulse::index;
    Query query; query.session_id = 55;
    IndexClient local;
    IndexClientTestAccess::Initialize(local, window, kMain);
    local.SearchAsync(query, 100);
    pulse::ipc::PayloadWriter hostile;
    hostile.PutU32(0); hostile.PutU32(UINT32_MAX);
    IndexClientTestAccess::Respond(local, 100, hostile.data());
    SearchResult result;
    Check(Posted(window, kMain, 100) && local.TakeResult(100, result) &&
          result.error == ERROR_INVALID_DATA && result.incomplete && result.hits.empty(),
          "tiny local frame with huge count produces a bounded error terminal");
    IndexClientTestAccess::Respond(local, 100, Response(1, 0, true));
    Check(Posted(window, kMain, 100) && local.Status().find(L"不完整") != std::wstring::npos,
          "incomplete response exposes query warning");
    IndexClientTestAccess::Respond(local, 100, Response(2, 0, false));
    Check(Posted(window, kMain, 100) && local.Status() == L"索引未连接", "complete refresh restores underlying status");
    pulse::ipc::PayloadWriter volumes;
    volumes.PutU32(0); volumes.PutString(L"fixture"); volumes.PutU32(UINT32_MAX);
    IndexClientTestAccess::Volumes(local, volumes.data());
    std::vector<VolumeInfo> known; std::vector<std::wstring> excluded;
    Check(!local.GetScope(known, excluded), "malformed volume count is rejected before scope publication");
    volumes = {};
    volumes.PutU32(0); volumes.PutString(L"fixture"); volumes.PutU32(0); volumes.PutU32(UINT32_MAX);
    IndexClientTestAccess::Volumes(local, volumes.data());
    Check(!local.GetScope(known, excluded), "malformed excluded-path count is rejected before publication");
    local.Stop();

    const auto full = Response(1, 0, false), revision = Response(2, 0, false);
    Check(SearchResponseHash(full) == SearchResponseHash(revision) &&
          SearchResponseHash(full) != SearchResponseHash(Response(1, 0, true)) &&
          SearchResponseHash(full) != SearchResponseHash(Response(1, ERROR_BUSY, false)),
          "subscription equality excludes only revision and includes both quality fields");
    SearchResult decoded;
    auto partial = full; partial.pop_back();
    Check(!DecodeSearchResponse(partial.data(), partial.size(), decoded), "partial quality trailer is rejected");
    pulse::ipc::PayloadWriter too_many;
    too_many.PutU32(0); too_many.PutU32(static_cast<uint32_t>(kSearchPageCap + 1));
    Check(!DecodeSearchResponse(too_many.data().data(), too_many.data().size(), decoded), "business page count limit is enforced");

    NetworkAgentClient network;
    SearchClientRoutingTestPeer::Initialize(network, window, kMain);
    network.SearchAsync(query, 200);
    SearchClientRoutingTestPeer::Respond(network, query, 200, false, 0, {}, ERROR_BROKEN_PIPE);
    Check(Posted(window, kMain, 200) && network.TakeResult(200, result) &&
          result.error == ERROR_BROKEN_PIPE && result.incomplete, "network transport failure publishes terminal");
    pulse::ipc::PayloadWriter legacy;
    legacy.PutU32(0); legacy.PutString(L"legacy timeout");
    SearchClientRoutingTestPeer::Respond(network, query, 200, true, agent::RSP_RESULT, legacy.data());
    Check(Posted(window, kMain, 200) && network.TakeResult(200, result) && result.error && result.incomplete,
          "legacy network error response publishes terminal");
    SearchClientRoutingTestPeer::Respond(network, query, 200, true, agent::RSP_SEARCH, Response(3, ERROR_BUSY, true));
    Check(Posted(window, kMain, 200) && network.TakeResult(200, result) && result.error == ERROR_BUSY,
          "typed network overload error survives decoding");
    SearchClientRoutingTestPeer::Respond(network, query, 200, true, agent::RSP_SEARCH, hostile.data());
    Check(Posted(window, kMain, 200) && network.TakeResult(200, result) && result.error == ERROR_INVALID_DATA,
          "malformed network response publishes terminal without oversized reserve");
    network.SearchAsync(query, 201);
    SearchClientRoutingTestPeer::Respond(network, query, 200, false, 0, {}, ERROR_TIMEOUT);
    Check(!Posted(window, kMain, 200), "stale network failure is discarded");
    network.CancelSession(query.session_id);
    SearchClientRoutingTestPeer::Respond(network, query, 201, false, 0, {}, ERROR_TIMEOUT);
    Check(!Posted(window, kMain, 201), "cancelled network failure is discarded");
    network.Stop();
}
template<class Client, class Peer>
void Exercise(const char* name, HWND main, HWND popup) {
    std::printf("-- %s --\n", name);
    Client client;
    Peer::Initialize(client, main, kMain);
    pulse::index::Query main_query, popup_query;
    main_query.session_id = 1; popup_query.session_id = 0x50554c534547534full;
    // Two windows may have the same local generation. Shared request ids come
    // from one owner counter instead, so neither result overwrites the other.
    uint32_t counter = 7;
    const uint32_t main_id = ++counter, popup_id = ++counter;
    client.SearchAsync(main_query, main_id);
    client.SearchAsync(popup_query, popup_id, popup, kPopup);
    Peer::Publish(client, popup_id); Peer::Publish(client, main_id);
    Check(Posted(main, kMain, main_id) && Posted(popup, kPopup, popup_id),
        "default and explicit routes deliver to their own windows");
    pulse::index::SearchResult result;
    Check(!client.TakeResult(popup_id, result) && !client.TakeResult(main_id, result, popup),
        "wrong consumer cannot drain another window's result");
    Check(client.TakeResult(main_id, result) && client.TakeResult(popup_id, result, popup),
        "default TakeResult stays compatible and popup result remains available");
    Peer::Publish(client, popup_id); Peer::Publish(client, main_id);
    client.CancelSession(popup_query.session_id);
    Check(!client.TakeResult(popup_id, result, popup) && client.TakeResult(main_id, result),
        "popup cancellation discards only its own pending result");
    Check(Posted(popup, kPopup, popup_id), "cancellation tolerates an already queued notification");
    Peer::Publish(client, popup_id);
    Check(!Posted(popup, kPopup, popup_id), "late cancelled response cannot post to popup");
    client.SearchAsync(main_query, ++counter);
    Peer::Publish(client, counter - 2);
    Check(!client.TakeResult(counter - 2, result), "replaced request rejects stale results");
    Peer::Publish(client, counter);
    Check(client.TakeResult(counter, result), "main session continues after popup cancellation");
    const HWND closing = CreateWindowExW(0, L"STATIC", L"closing", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(closing != nullptr, "disposable popup HWND created");
    if (closing) {
        client.SearchAsync(popup_query, ++counter, closing, kPopup);
        client.CancelSession(popup_query.session_id);
        DestroyWindow(closing);
        Peer::Publish(client, counter);
        Check(!client.TakeResult(counter, result, closing), "late response after popup destruction has no result route");
    }
    client.Stop();
    MSG queued{};
    while (PeekMessageW(&queued, main, kMain, kMain, PM_REMOVE)) {}
    while (PeekMessageW(&queued, popup, kPopup, kPopup, PM_REMOVE)) {}
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--transport-liveness-only")
        return pulse::index::RunIndexClientIoRegressions();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--writer-idle-only") {
        WriterIdle();
        return failures ? 1 : 0;
    }
    const HWND main = CreateWindowExW(0, L"STATIC", L"main", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    const HWND popup = CreateWindowExW(0, L"STATIC", L"popup", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(main && popup, "isolated message-only windows created without user data or agents");
    if (main && popup) {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--live-only") LiveSearch(main);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--boundaries-only") Boundaries(main);
        else {
            Exercise<pulse::index::IndexClient, pulse::index::IndexClientTestAccess>("index", main, popup);
            Exercise<pulse::index::NetworkAgentClient, pulse::index::SearchClientRoutingTestPeer>("network", main, popup);
        }
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
    }
    if (popup) DestroyWindow(popup);
    if (main) DestroyWindow(main);
    return failures ? 1 : 0;
}
