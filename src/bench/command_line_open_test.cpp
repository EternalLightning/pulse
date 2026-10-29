#include "../app/startup_launch.h"
#include "../app/single_instance_coordinator.h"
#include "../app/default_file_manager.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>

namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
}

int wmain() {
    using pulse::app::CaptureStartupOpenPath;
    using pulse::app::SingleInstanceCoordinator;
    for (const auto* option : {L"-Embedding", L"--unknown", L"/Embedding", L"/automation"}) {
        std::wstring path;
        CaptureStartupOpenPath(option, true, path);
        Check(path.empty(), "a final switch does not become an open path");
        path = L"C:\\folder with spaces";
        CaptureStartupOpenPath(option, true, path);
        Check(path == L"C:\\folder with spaces", "a switch does not replace the earlier path");
    }
    std::wstring path;
    CaptureStartupOpenPath(L"C:\\folder with spaces", false, path);
    Check(path == L"C:\\folder with spaces", "a quoted argv path keeps its spaces");
    CaptureStartupOpenPath(L"D:\\last", true, path);
    Check(path == L"D:\\last", "the final positional path keeps existing precedence");

    wchar_t original[32768]{};
    if (!GetCurrentDirectoryW(ARRAYSIZE(original), original)) return 2;
    const auto fixture = std::filesystem::absolute(L"bench_data/command-line-open") /
        std::to_wstring(GetCurrentProcessId());
    const auto caller = fixture / L"caller with spaces";
    const auto receiver = fixture / L"receiver";
    std::filesystem::create_directories(caller);
    std::filesystem::create_directories(receiver);
    Check(SetCurrentDirectoryW(caller.c_str()) != FALSE, "set only this test process caller directory");
    std::wstring forwarded;
    Check(SingleInstanceCoordinator::PrepareOpenPath(L".", forwarded) &&
        forwarded == caller.wstring(), "relative dot resolves in the caller before IPC");
    Check(SetCurrentDirectoryW(receiver.c_str()) != FALSE, "simulate a receiver with a different directory");
    COPYDATASTRUCT data{};
    data.dwData = SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((forwarded.size() + 1) * sizeof(wchar_t));
    data.lpData = forwarded.data();
    std::wstring decoded;
    Check(SingleInstanceCoordinator::DecodeOpenPath(&data, decoded) && decoded == caller.wstring(),
        "decoded IPC path retains caller location after CWD changes");
    Check(SingleInstanceCoordinator::PrepareOpenPath(L"\"" + caller.wstring() + L"\"", forwarded) &&
        forwarded == caller.wstring(), "quoted full path is normalized without losing spaces");
    for (const auto* special : {L"", pulse::app::kThisPcParsingName, L"shell:MyComputerFolder", L"pulse:recycle"}) {
        Check(SingleInstanceCoordinator::PrepareOpenPath(special, forwarded) && forwarded == special,
            "activation and supported namespace arguments remain intact");
    }
    Check(!SingleInstanceCoordinator::PrepareOpenPath(std::wstring(L"a\0b", 3), forwarded),
        "embedded NUL is rejected before sending");
    Check(SetCurrentDirectoryW(original) != FALSE, "restore this test process directory");
    std::error_code error;
    std::filesystem::remove_all(fixture, error);
    Check(!error, "remove the isolated fixture");
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
