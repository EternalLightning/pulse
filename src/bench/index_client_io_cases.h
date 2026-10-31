// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <utility>

namespace pulse::index {
namespace index_io_test {
inline bool Write(HANDLE pipe, const void* data, DWORD bytes) {
    return ipc::PipeWrite(pipe, data, bytes, {.overlapped = false});
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE Release() { return std::exchange(value, nullptr); }
};
struct Pair {
    Handle server, client;
    bool Open() {
        static uint32_t sequence = 0;
        const auto name = L"\\\\.\\pipe\\PulseIndexIoTest-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(++sequence);
        server.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr);
        if (server.value == INVALID_HANDLE_VALUE) return false;
        client.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        return client.value != INVALID_HANDLE_VALUE &&
            (ConnectNamedPipe(server.value, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
    }
};
inline bool WaitStartedWrite(HANDLE server) {
    const auto deadline = GetTickCount64() + 1000;
    DWORD available = 0;
    while (GetTickCount64() < deadline) {
        if (!PeekNamedPipe(server, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available >= sizeof(ipc::MsgHeader)) return true;
        Sleep(1);
    }
    return false;
}
}

inline int RunIndexClientIoRegressions() {
    using namespace index_io_test;
    using Access = IndexClientTestAccess;
    int failures = 0;
    const auto check = [&](bool success, const char* label) {
        std::printf("[%s] %s\n", success ? "PASS" : "FAIL", label);
        if (!success) ++failures;
    };
    {
        Pair pair;
        check(pair.Open(), "open isolated Index header/body cancellation fixture");
        if (pair.client.value == INVALID_HANDLE_VALUE) return 2;
        IndexClient client;
        Access::AttachIo(client, pair.client.Release());
        Handle header_seen(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle continue_body(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle read_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle stop_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Access::BodyGate(client, [&] { SetEvent(header_seen.value); WaitForSingleObject(continue_body.value, 2000); });
        bool received = false; DWORD error = ERROR_SUCCESS;
        Access::ReadIo(client, read_done.value, received, error);
        const auto header = MakeIndexHdr(RSP_IDX_STATUS, 1, 4);
        check(Write(pair.server.value, &header, sizeof(header)) &&
            WaitForSingleObject(header_seen.value, 1000) == WAIT_OBJECT_0,
            "pause Index after complete header and before body I/O starts");
        std::thread stopper([&] { client.Stop(); SetEvent(stop_done.value); });
        const auto deadline = GetTickCount64() + 1000;
        while (Access::Running(client) && GetTickCount64() < deadline) Sleep(1);
        Sleep(20);
        SetEvent(continue_body.value);
        const bool bounded = WaitForSingleObject(stop_done.value, 250) == WAIT_OBJECT_0;
        if (!bounded) DisconnectNamedPipe(pair.server.value);
        stopper.join();
        check(bounded && !received && (error == ERROR_CANCELLED || error == ERROR_OPERATION_ABORTED),
            "Index Stop before body admission finishes while peer stays alive and silent");
    }
    {
        Pair pair;
        check(pair.Open(), "open isolated Index writer backpressure fixture");
        if (pair.client.value == INVALID_HANDLE_VALUE) return 2;
        IndexClient client;
        Access::AttachIo(client, pair.client.Release());
        Handle write_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle disconnect_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        bool sent = false;
        Access::WriteIo(client, write_done.value, sent);
        check(WaitStartedWrite(pair.server.value) && WaitForSingleObject(write_done.value, 20) == WAIT_TIMEOUT,
            "Index write fills pipe while peer remains connected and does not read");
        Access::DisconnectIo(client, disconnect_done.value);
        const bool bounded = WaitForSingleObject(disconnect_done.value, 250) == WAIT_OBJECT_0;
        if (!bounded) DisconnectNamedPipe(pair.server.value);
        client.Stop();
        check(bounded && !sent && WaitForSingleObject(write_done.value, 0) == WAIT_OBJECT_0,
            "Index Disconnect signals cancellation before waiting for the blocked writer lock");
    }
    for (const bool partial_header : {true, false}) {
        Pair pair;
        check(pair.Open(), "open isolated Index incomplete frame fixture");
        if (pair.client.value == INVALID_HANDLE_VALUE) return 2;
        IndexClient client;
        Access::AttachIo(client, pair.client.Release());
        Handle read_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        bool received = false; DWORD error = ERROR_SUCCESS;
        Access::ReadIo(client, read_done.value, received, error);
        const auto header = MakeIndexHdr(RSP_IDX_STATUS, 1, 4);
        check(Write(pair.server.value, &header, partial_header ? 1 : sizeof(header)),
            "peer sends an incomplete Index frame and remains alive");
        const bool completed = WaitForSingleObject(read_done.value, 6500) == WAIT_OBJECT_0;
        client.Stop();
        check(completed && !received && error == ERROR_TIMEOUT,
            partial_header ? "Index partial header expires under the frame deadline" :
                "Index complete header does not grant an unbounded body wait");
    }
    {
        Pair pair;
        check(pair.Open(), "open isolated normal Index response fixture");
        if (pair.client.value == INVALID_HANDLE_VALUE) return 2;
        IndexClient client;
        Access::AttachIo(client, pair.client.Release());
        Handle read_done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        bool received = false; DWORD error = ERROR_SUCCESS;
        Access::ReadIo(client, read_done.value, received, error);
        const auto header = MakeIndexHdr(RSP_IDX_STATUS, 1, 4);
        const uint32_t payload = 17;
        check(Write(pair.server.value, &header, sizeof(header)) &&
            Write(pair.server.value, &payload, sizeof(payload)) &&
            WaitForSingleObject(read_done.value, 1000) == WAIT_OBJECT_0,
            "normal Index frame completes with intact header and body");
        client.Stop();
        check(received && error == ERROR_SUCCESS, "Index lifecycle changes retain normal frame success");
    }
    return failures ? 1 : 0;
}
}
