#include "../ops/operation_elevation_transport.h"

#include <cstdio>
#include <array>
#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>

#ifndef PULSE_OPERATION_ELEVATION_TESTING
#error This target must define PULSE_OPERATION_ELEVATION_TESTING.
#endif

namespace {
using namespace pulse::ops;

OperationElevationResult Execute(void*, std::span<const uint8_t> request,
                                OperationElevationWorkerChannel& channel) {
    OperationElevationResult result;
    if (request.size() != 1 || request[0] > 5) {
        result.error = E_INVALIDARG;
        return result;
    }
    const std::array<uint8_t, 2> first{1, 0};
    std::vector<uint8_t> control;
    const bool reported = channel.ReportProgress(first, &control);
    if (!reported && request[0] != 2 && request[0] != 3) return {E_ABORT, true, false, {}};
    if (request[0] == 5) return {control == std::vector<uint8_t>{42} ? S_OK : E_FAIL, false, false, control};
    if (request[0] == 1) ExitProcess(71);
    if (request[0] == 2) {
        const ULONGLONG started = GetTickCount64();
        while (!channel.IsCancelled() && GetTickCount64() - started < 10000) Sleep(5);
        result.cancelled = channel.IsCancelled();
        result.error = result.cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_FAIL;
        result.response = {1, 0};
        return result;
    }
    if (request[0] == 3) {
        // Represents a completed mutation followed by worker death while
        // acknowledging a cancel: the parent must keep the proven first item.
        const ULONGLONG started = GetTickCount64();
        while (!channel.IsCancelled() && GetTickCount64() - started < 10000) Sleep(5);
        ExitProcess(72);
    }
    if (request[0] == 4) {
        std::vector<uint8_t> too_large(kOperationElevationMaxPayload + 1, 7);
        const bool accepted = channel.ReportProgress(too_large);
        result.error = accepted ? E_FAIL : HRESULT_FROM_WIN32(ERROR_BAD_LENGTH);
        result.cancelled = channel.IsCancelled();
        result.response = {1};
        return result;
    }
    const std::array<uint8_t, 2> complete{1, 1};
    if (!channel.ReportProgress(complete)) return {E_ABORT, true, false, {}};
    result.error = S_OK;
    result.response = {1, 1};
    return result;
}

struct Observer {
    std::vector<std::vector<uint8_t>> snapshots;
    std::atomic<bool> progress_received{false};
    bool cancel_after_progress = false;
    bool cancel_immediately = false;
    std::vector<uint8_t> control;
    static std::vector<uint8_t> Progress(void* opaque, std::span<const uint8_t> bytes) {
        auto& self = *static_cast<Observer*>(opaque);
        self.snapshots.emplace_back(bytes.begin(), bytes.end());
        self.progress_received.store(true);
        return self.control;
    }
    static bool Cancelled(void* opaque) {
        const auto& self = *static_cast<Observer*>(opaque);
        return self.cancel_immediately || (self.cancel_after_progress && self.progress_received.load());
    }
    OperationElevationCallbacks Callbacks() { return {this, &Progress, &Cancelled}; }
};

int failures = 0;
void Check(bool success, const char* description, const OperationElevationResult* result = nullptr) {
    std::printf("[%s] %s", success ? "PASS" : "FAIL", description);
    if (!success && result) std::printf(" hr=0x%08lx cancelled=%d unknown=%d bytes=%zu",
        static_cast<unsigned long>(result->error), result->cancelled, result->outcome_unknown,
        result->response.size());
    std::puts("");
    if (!success) ++failures;
}
std::wstring SelfPath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return path;
}
struct PathFixture {
    std::filesystem::path root;
    bool Create() {
        root = std::filesystem::absolute(L"bench_data") /
            (L"pulse-elevation-path-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        std::filesystem::create_directories(root / L"Program Files" / L"Pulse", error);
        if (error) return false;
        std::filesystem::create_directories(root / L"Other", error);
        if (error) return false;
        return std::ofstream(root / L"Program Files" / L"Pulse" / L"Pulse.exe").good() &&
            std::ofstream(root / L"Program Files" / L"Pulse" / L"Pulse.Shell.exe").good() &&
            std::ofstream(root / L"Other" / L"Pulse.Shell.exe").good();
    }
    ~PathFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        Check(!error, "remove isolated allowlist fixture files");
    }
};
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--file-operation-worker")
        return pulse::ops::RunOperationElevationWorker(argc, argv, {nullptr, &Execute});
    const auto executable = SelfPath();
    Check(!executable.empty(), "locate isolated fixture executable");
    if (executable.empty()) return 1;
    std::puts("Same-integrity transport fixture: no UAC consent or elevated filesystem verification.");
    Observer normal;
    const std::array<uint8_t, 1> request{0};
    auto result = RunOperationElevationForTest(executable, request, normal.Callbacks());
    Check(result.error == S_OK && !result.cancelled && !result.outcome_unknown &&
        result.response == std::vector<uint8_t>({1, 1}), "authenticated terminal result", &result);
    Check(normal.snapshots == std::vector<std::vector<uint8_t>>({{1, 0}, {1, 1}}),
        "ordered cumulative progress snapshots");

    Observer conflict;
    conflict.control = {42};
    const std::array<uint8_t, 1> conflict_request{5};
    result = RunOperationElevationForTest(executable, conflict_request, conflict.Callbacks());
    Check(result.error == S_OK && result.response == std::vector<uint8_t>{42} && !result.outcome_unknown,
        "progress control acknowledgement reaches executor", &result);

    Observer died;
    const std::array<uint8_t, 1> die_request{1};
    result = RunOperationElevationForTest(executable, die_request, died.Callbacks());
    Check(FAILED(result.error) && result.outcome_unknown && result.response.empty(),
        "worker death reports unknown outcome without fabricated terminal", &result);
    Check(died.snapshots == std::vector<std::vector<uint8_t>>({{1, 0}}),
        "worker death preserves proven progress");

    Observer cancelled;
    cancelled.cancel_after_progress = true;
    const std::array<uint8_t, 1> cancel_request{2};
    result = RunOperationElevationForTest(executable, cancel_request, cancelled.Callbacks());
    Check(result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.cancelled &&
        !result.outcome_unknown && result.response == std::vector<uint8_t>({1, 0}),
        "cancel reaches executor and returns authenticated partial terminal", &result);

    Observer cancel_died;
    cancel_died.cancel_after_progress = true;
    const std::array<uint8_t, 1> cancel_die_request{3};
    result = RunOperationElevationForTest(executable, cancel_die_request, cancel_died.Callbacks());
    Check(FAILED(result.error) && result.cancelled && result.outcome_unknown &&
        cancel_died.snapshots == std::vector<std::vector<uint8_t>>({{1, 0}}),
        "death after cancel preserves partial progress and remains unknown", &result);

    Observer immediate;
    immediate.cancel_immediately = true;
    result = RunOperationElevationForTest(executable, request, immediate.Callbacks());
    Check(result.cancelled && !result.outcome_unknown && immediate.snapshots.empty(),
        "prelaunch cancellation dispatches no operation", &result);
    result = RunOperationElevationForTest(executable, {});
    Check(result.error == E_INVALIDARG && !result.outcome_unknown, "reject empty request", &result);
    std::vector<uint8_t> oversized(kOperationElevationMaxPayload + 1, 0);
    result = RunOperationElevationForTest(executable, oversized);
    Check(result.error == E_INVALIDARG && !result.outcome_unknown, "reject oversized request before launch", &result);
    const std::array<uint8_t, 1> bad_progress{4};
    result = RunOperationElevationForTest(executable, bad_progress);
    Check(result.error == HRESULT_FROM_WIN32(ERROR_BAD_LENGTH) && result.cancelled &&
        !result.outcome_unknown, "oversized progress is refused and cancels executor", &result);
    result = RunElevatedOperation(executable, nullptr, request);
    Check(result.error == E_ACCESSDENIED && !result.outcome_unknown,
        "production entry rejects fixture path before UAC", &result);
    PathFixture paths;
    Check(paths.Create(), "create existing isolated allowlist fixture files");
    const std::wstring production_parent = (paths.root / L"Program Files" / L"Pulse" / L"Pulse.exe").wstring();
    const std::wstring production_worker = (paths.root / L"Program Files" / L"Pulse" / L"Pulse.Shell.exe").wstring();
    Check(OperationElevationPathsForTest(production_parent, production_worker, false) &&
        !OperationElevationPathsForTest(production_parent, production_parent, false) &&
        !OperationElevationPathsForTest(production_parent, (paths.root / L"Other" / L"Pulse.Shell.exe").wstring(), false) &&
        !OperationElevationPathsForTest(production_worker, production_worker, false),
        "production elevation accepts only main parent and its sibling Shell worker");
    Check(OperationElevationPathsForTest(executable, executable, true) &&
        !OperationElevationPathsForTest(executable, production_worker, true),
        "same-image transport fixture exception stays within explicit testing mode");
    wchar_t executable_arg[] = L"fixture.exe";
    wchar_t worker_arg[] = L"--file-operation-worker";
    wchar_t invalid_pipe[] = L"\\\\server\\pipe\\Pulse.OperationElevation.bad";
    wchar_t pid[] = L"1";
    wchar_t created[] = L"1";
    wchar_t* invalid_argv[]{executable_arg, worker_arg, invalid_pipe, pid, created};
    Check(RunOperationElevationWorker(5, invalid_argv, {nullptr, &Execute}) == ERROR_INVALID_PARAMETER,
        "worker rejects remote or noncanonical pipe names");
    wchar_t canonical_pipe[] = L"\\\\.\\pipe\\Pulse.OperationElevation.0000000000000000000000000000000000000000000000000000000000000000";
    invalid_argv[2] = canonical_pipe;
    wchar_t overflow[] = L"18446744073709551616";
    invalid_argv[3] = overflow;
    Check(RunOperationElevationWorker(5, invalid_argv, {nullptr, &Execute}) == ERROR_INVALID_PARAMETER,
        "worker rejects malformed launch arguments");
    std::wstring own_pid = std::to_wstring(GetCurrentProcessId());
    invalid_argv[3] = own_pid.data();
    Check(RunOperationElevationWorker(5, invalid_argv, {nullptr, &Execute}) == ERROR_ACCESS_DENIED,
        "worker rejects a launcher creation-time mismatch");
    std::printf("Transport fixture: %d failure(s).\n", failures);
    return failures ? 1 : 0;
}
