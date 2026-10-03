#include "../index/network_agent_protocol.h"
#include <iostream>
#include <string>
namespace {
int failures = 0;
void Check(bool value, const char* label) { std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!value) ++failures; }
HANDLE Connect(const std::wstring& name) {
    const auto deadline = GetTickCount64() + 5000;
    do {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 50); else Sleep(10);
    } while (GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}
}
int wmain() {
    using namespace pulse::index;
    std::cout << std::unitbuf;
    const auto tag = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto name = L"\\\\.\\pipe\\PulseNetworkIndex.Test." + tag;
    const auto exe = transport::SiblingIndexImage();
    auto command = L"\"" + exe + L"\" --test-network-agent " + tag;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process) != FALSE, "isolated network host starts without touching roots or user cache");
    if (!process.hProcess) return 1;
    CloseHandle(process.hThread); transport::Handle child(process.hProcess);
    transport::Handle idle(Connect(name));
    Check(idle.get() != INVALID_HANDLE_VALUE, "idle no-frame client connects");
    transport::Handle responsive(Connect(name));
    Check(responsive.get() != INVALID_HANDLE_VALUE, "idle connection does not block a second client");
    Check(transport::VerifyServer(responsive.get(), exe, transport::Identity::Current()), "client authenticates real host PID image and token");
    Check(!transport::VerifyServer(responsive.get(), exe + L".wrong", transport::Identity::Current()), "client rejects wrong server executable identity");
    auto request = agent::MakeHeader(agent::REQ_STATUS, 12, 0);
    pulse::ipc::MsgHeader reply{};
    const auto deadline = GetTickCount64() + 1500;
    bool received = transport::Transfer(responsive.get(), &request, sizeof(request), true, deadline) &&
        transport::Transfer(responsive.get(), &reply, sizeof(reply), false, deadline) &&
        reply.magic == agent::kMagic && reply.type == agent::RSP_STATUS && reply.request_id == 12 && reply.payload_size < 65536;
    std::vector<BYTE> bytes(received ? reply.payload_size : 0);
    if (received && !bytes.empty()) received = transport::Transfer(responsive.get(), bytes.data(), static_cast<DWORD>(bytes.size()), false, deadline);
    Check(received, "authenticated status completes while another client sends no frame");
    // An advertised body is never delivered. The host must close the connection
    // on the same finite header+payload deadline and still accept a new client.
    transport::Handle partial(Connect(name));
    request = agent::MakeHeader(agent::REQ_SEARCH, 13, 256);
    Check(transport::Transfer(partial.get(), &request, sizeof(request), true, GetTickCount64() + 500), "partial frame header is sent");
    BYTE byte = 0; const auto started = GetTickCount64();
    const bool body_read = transport::Transfer(partial.get(), &byte, 1, false, started + 4000);
    Check(!body_read && GetTickCount64() - started < 3500, "stalled frame is disconnected within server deadline");
    transport::Handle control(Connect(name));
    request = agent::MakeHeader(0x7fff0001, 14, 0);
    Check(transport::Transfer(control.get(), &request, sizeof(request), true, GetTickCount64() + 500), "isolated fixture shutdown sent");
    const auto stopped = WaitForSingleObject(child.get(), 5000);
    Check(stopped == WAIT_OBJECT_0, "network host joins finite client workers on shutdown");
    if (stopped != WAIT_OBJECT_0) TerminateProcess(child.get(), 1);
    return failures ? 1 : 0;
}
