// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../shell_host/ctx_session_process.h"

#include <cstdio>
#include <map>
#include <mutex>

namespace {
using namespace pulse::ipc;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Frame(HANDLE pipe, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload) {
    MsgHeader header; header.type = type; header.request_id = id;
    header.payload_size = static_cast<uint32_t>(payload.size());
    DWORD written = 0;
    return WriteFile(pipe, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
        WriteFile(pipe, payload.data(), header.payload_size, &written, nullptr) && written == header.payload_size;
}
int Worker(DWORD parent, uint32_t id) {
    Handle pipe;
    const auto name = pulse::shell::ContextWorkerPipeName(parent, id);
    pipe.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
        1, 4096, 4096, 0, nullptr);
    if (pipe.value == INVALID_HANDLE_VALUE || (!ConnectNamedPipe(pipe.value, nullptr) &&
        GetLastError() != ERROR_PIPE_CONNECTED)) return 2;
    MsgHeader header{}; DWORD read = 0;
    std::vector<uint8_t> payload;
    uint32_t current = 0, invocations = 0;
    while (ReadFile(pipe.value, &header, sizeof(header), &read, nullptr) && read == sizeof(header)) {
        payload.resize(header.payload_size);
        if (header.payload_size && !ReadFile(pipe.value, payload.data(), header.payload_size, &read, nullptr)) return 4;
        if (header.type == REQ_CTX_QUERY) {
            current = header.request_id;
            if (current == 1) continue;
            PayloadWriter writer;
            writer.PutU32(current); writer.PutU32(current == 2 || current == 3 ? CTX_ITEMS_PARTIAL : 0);
            writer.PutU32(current >= 3 ? 1 : 0);
            if (current >= 3) {
                writer.PutU32(7); writer.PutU32(CTX_ITEM_ENABLED); writer.PutString(L"fixture.ready");
                writer.PutString(std::to_wstring(GetCurrentProcessId())); writer.PutString(L""); writer.PutString(L"");
            }
            writer.PutStringArray({});
            if (!Frame(pipe.value, RSP_CTX_ITEMS, current, writer.data())) return 5;
        } else if (header.type == REQ_CTX_CLOSE) {
            if (current != 14 && !Frame(pipe.value, RSP_PONG, current, {})) return 6;
        } else if (header.type == REQ_CTX_INVOKE) {
            PayloadWriter done; done.PutU32(S_OK); done.PutU32(0);
            done.PutString(std::to_wstring(current) + L":" + std::to_wstring(++invocations));
            if (!Frame(pipe.value, RSP_DONE, header.request_id, done.data())) return 7;
        }
    }
    return 0;
}

int ReuseTest(const wchar_t* executable, bool stale_only = false) {
    using namespace pulse::shell;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    std::mutex mutex;
    std::map<uint32_t, DWORD> pids;
    std::map<uint32_t, uint32_t> counts;
    std::map<uint32_t, uint32_t> invocation_results;
    std::map<uint32_t, std::wstring> invocation_details;
    ContextSessionProcesses processes(executable, [&](uint32_t type, uint32_t id, const auto& payload) {
        PayloadReader reader(payload.data(), payload.size());
        uint32_t session = 0, flags = 0, count = 0, item = 0;
        std::wstring verb, text;
        if (type == RSP_DONE && reader.GetU32(flags)) {
            reader.GetU32(count);
            reader.GetString(text);
            std::lock_guard lock(mutex);
            invocation_results[id] = flags;
            invocation_details[id] = std::move(text);
        } else if (type == RSP_CTX_ITEMS && reader.GetU32(session) && reader.GetU32(flags) && reader.GetU32(count)) {
            if (count && (!reader.GetU32(item) || !reader.GetU32(flags) || !reader.GetString(verb) || !reader.GetString(text))) return;
            std::lock_guard lock(mutex);
            counts[id] = count;
            if (count) pids[id] = wcstoul(text.c_str(), nullptr, 10);
        }
    }, {stale_only ? 3000U : 400U, 2});
    const auto wait = [&](const auto& predicate, DWORD timeout = 3000) {
        const auto deadline = GetTickCount64() + timeout;
        do {
            if (predicate()) return true;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        return false;
    };
    const auto query = [&](uint32_t id) {
        processes.Query(id, {});
        return wait([&] { std::lock_guard lock(mutex); return pids.contains(id); });
    };
    check(query(10), "first isolated query completes");
    processes.Close(10);
    check(wait([&] { return processes.IdleProcesses() == 1; }), "cleanup acknowledgement admits one idle worker");
    check(query(11), "next session uses the released worker");
    {
        std::lock_guard lock(mutex);
        std::printf("[DIAG] released worker PID=%lu next-session PID=%lu\n", pids[10], pids[11]);
        check(pids[10] && pids[10] == pids[11], "acknowledged queries use the same process with different session IDs");
    }
    processes.Close(10);
    PayloadWriter invocation;
    invocation.PutU32(11); invocation.PutU32(7);
    invocation.PutString(L"fixture.ready"); invocation.PutString(L"");
    processes.Invoke(10, 110, invocation.data());
    check(wait([&] { std::lock_guard lock(mutex); return invocation_results.contains(110); }) &&
        processes.ActiveProcesses() == 1 && processes.IdleProcesses() == 0,
        "stale close and invoke cannot address the reused process under its previous session ID");
    {
        std::lock_guard lock(mutex);
        const auto stale = invocation_results.find(110);
        check(stale != invocation_results.end() && stale->second == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND)),
            "stale session invoke returns the exact not-found failure instead of worker success");
    }
    processes.Invoke(11, 111, invocation.data());
    check(wait([&] { std::lock_guard lock(mutex); return invocation_results.contains(111) && invocation_results.at(111) == S_OK &&
        invocation_details[111] == L"11:1"; }),
        "the current session still invokes after rejecting its previous token");
    processes.Close(11);
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "invoked reused worker physically retires");
    if (stale_only) return failures ? 1 : 0;
    check(query(17), "query-count fixture starts a fresh worker");
    processes.Close(17);
    check(wait([&] { return processes.IdleProcesses() == 1; }), "query-count fixture releases its first session");
    check(query(18), "query-count fixture reuses its worker for a second query");
    processes.Close(18);
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "query count limit physically retires the warm process");
    check(query(12), "query after retirement recovers with a fresh process");
    processes.Close(12);
    check(wait([&] { return processes.IdleProcesses() == 1; }), "fresh released query becomes idle");
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "idle deadline physically reclaims the worker");
    check(query(14), "unresponsive close fixture completes its query");
    processes.Close(14);
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "missing cleanup acknowledgement kills the worker within a bounded wait");
    check(processes.IdleProcesses() == 0, "unacknowledged worker is never reused");
    check(query(16), "synthetic invocation fixture completes its query");
    processes.Invoke(16, 116, {});
    check(wait([&] { std::lock_guard lock(mutex); return invocation_results.contains(116) && invocation_results[116] == S_OK; }),
        "synthetic command completes before retirement");
    processes.Close(16);
    check(wait([&] { return processes.ActiveProcesses() == 0; }) && processes.IdleProcesses() == 0,
        "invoked processes are physically retired and never become idle");
    for (uint32_t id = 20; id < 24; ++id) check(query(id), "concurrent isolated query admitted");
    processes.Query(24, {});
    check(wait([&] { std::lock_guard lock(mutex); return counts.contains(24) && counts[24] == 0; }) &&
        processes.ActiveProcesses() == ContextSessionProcesses::kProcessLimit,
        "fifth concurrent menu is rejected without exceeding the physical process limit");
    for (uint32_t id = 20; id < 24; ++id) processes.Close(id);
    check(wait([&] { return processes.ActiveProcesses() <= 1; }), "concurrent cleanup retains at most one idle process");
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "remaining concurrent idle worker expires");
    check(query(15), "query recovers after cleanup failure");
    processes.Close(15);
    check(wait([&] { return processes.IdleProcesses() == 1; }), "idle worker ready for shutdown");
    processes.Stop();
    check(wait([&] { return processes.ActiveProcesses() == 0; }), "Stop physically reclaims idle processes");
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 4 && wcscmp(argv[1], L"--context-worker") == 0)
        return Worker(wcstoul(argv[2], nullptr, 10), wcstoul(argv[3], nullptr, 10));
    if (argc == 2 && (wcscmp(argv[1], L"--reuse") == 0 || wcscmp(argv[1], L"--stale-session") == 0)) {
        wchar_t executable[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return 2;
        return ReuseTest(executable, wcscmp(argv[1], L"--stale-session") == 0);
    }
    using namespace pulse::shell;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    struct Result { bool final = false; bool partial = false; bool done = false; uint32_t count = 0; };
    std::mutex mutex;
    std::map<uint32_t, Result> results;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    ContextSessionProcesses processes(executable, [&](uint32_t type, uint32_t id, const auto& payload) {
        std::lock_guard lock(mutex);
        auto& result = results[id];
        PayloadReader reader(payload.data(), payload.size());
        uint32_t session = 0, flags = 0, count = 0;
        if (type == RSP_CTX_ITEMS && reader.GetU32(session) && reader.GetU32(flags) && reader.GetU32(count)) {
            result.partial |= (flags & CTX_ITEMS_PARTIAL) != 0;
            result.final |= !(flags & CTX_ITEMS_PARTIAL);
            result.count = count;
        } else if (type == RSP_DONE && reader.GetU32(flags)) result.done = flags == S_OK;
    }, {0, 0});
    const auto start = GetTickCount64();
    for (uint32_t id = 1; id <= 4; ++id) processes.Query(id, {});
    const auto wait = [&](const auto& predicate, DWORD timeout) {
        const auto deadline = GetTickCount64() + timeout;
        do {
            { std::lock_guard lock(mutex); if (predicate()) return true; }
            Sleep(20);
        } while (GetTickCount64() < deadline);
        return false;
    };
    check(wait([&] { return results[2].partial && results[3].partial && results[4].final; }, 3000),
        "real worker processes publish empty partial, ready partial and completed snapshots");
    check(wait([&] { return results[1].final && results[2].final && results[3].final; },
        ContextSessionProcesses::kQueryTimeoutMs + 2000), "initial and partial query hangs receive bounded final completion");
    std::printf("[INFO] query completion elapsed=%llu ms\n", GetTickCount64() - start);
    {
        std::lock_guard lock(mutex);
        check(results[1].count == 0 && results[2].count == 0 && results[3].count == 1,
            "timeout removes empty Loading queries and retains the exact ready command snapshot");
    }
    check(processes.ActiveProcesses() == 2, "empty hung workers are reclaimed while ready and completed sessions remain alive");
    for (uint32_t id : {3u, 4u}) {
        PayloadWriter invoke; invoke.PutU32(id); invoke.PutU32(7);
        invoke.PutString(L"fixture.ready"); invoke.PutString(L"");
        processes.Invoke(id, id + 100, invoke.data());
    }
    check(wait([&] { return results[103].done && results[104].done; }, 3000),
        "timed-out partial and normally completed sessions both invoke after the query deadline");
    for (uint32_t id = 1; id <= 4; ++id) processes.Close(id);
    const auto cleanup = GetTickCount64() + 3000;
    while (processes.ActiveProcesses() && GetTickCount64() < cleanup) Sleep(20);
    check(processes.ActiveProcesses() == 0, "all isolated workers physically exit on close");
    return failures ? 1 : 0;
}
