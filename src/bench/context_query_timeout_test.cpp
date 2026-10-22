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
    if (!ReadFile(pipe.value, &header, sizeof(header), &read, nullptr) || read != sizeof(header)) return 3;
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size && !ReadFile(pipe.value, payload.data(), header.payload_size, &read, nullptr)) return 4;
    if (id != 1) {
        PayloadWriter writer;
        writer.PutU32(id); writer.PutU32(id == 4 ? 0 : CTX_ITEMS_PARTIAL);
        writer.PutU32(id >= 3 ? 1 : 0);
        if (id >= 3) {
            writer.PutU32(7); writer.PutU32(CTX_ITEM_ENABLED); writer.PutString(L"fixture.ready");
            writer.PutString(std::to_wstring(GetCurrentProcessId())); writer.PutString(L""); writer.PutString(L"");
        }
        writer.PutStringArray({});
        if (!Frame(pipe.value, RSP_CTX_ITEMS, id, writer.data())) return 5;
    }
    while (ReadFile(pipe.value, &header, sizeof(header), &read, nullptr) && read == sizeof(header)) {
        payload.resize(header.payload_size);
        if (header.payload_size && !ReadFile(pipe.value, payload.data(), header.payload_size, &read, nullptr)) return 6;
        if (header.type == REQ_CTX_INVOKE) {
            PayloadWriter done; done.PutU32(S_OK); done.PutU32(0); done.PutString(L"fixture invoked");
            if (!Frame(pipe.value, RSP_DONE, header.request_id, done.data())) return 7;
        }
    }
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && wcscmp(argv[1], L"--context-worker") == 0)
        return Worker(wcstoul(argv[2], nullptr, 10), wcstoul(argv[3], nullptr, 10));
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
    });
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
