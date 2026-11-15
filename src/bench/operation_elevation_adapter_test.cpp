// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/operation_elevation_adapter.h"
#include "../ops/operation_elevation_transport.h"
#include "../ops/elevated_recycle_request.h"
#include "../shell_host/file_operations.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <objbase.h>

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) return pulse::ops::OperationElevationAdapter::RunWorker(argc, argv);
    const bool passed = pulse::ops::OperationElevationAdapter::CodecChecksForTest();
    std::printf("[%s] volume-label worker accepts only bounded labels at local drive roots, rejects ordinary operations and malformed payloads, and respects cancellation/API errors\n",
        passed ? "PASS" : "FAIL");
    if (!passed) return 1;
    namespace fs = std::filesystem;
    using namespace pulse;
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return 2;
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"recycle-worker-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64())));
    if (!fs::create_directory(root)) return 2;
    const auto first = root / L"first.txt", stale = root / L"stale.txt";
    std::ofstream(first) << "first worker bytes";
    std::ofstream(stale) << "stale worker bytes";
    FileIdentity identity;
    if (!ReadPathFileIdentity(first.wstring(), identity)) return 2;
    ipc::ShellOperationRequest request;
    request.kind = ipc::ShellOperationKind::Recycle;
    request.items = {{first.wstring(), {}, identity, {}}, {stale.wstring(), {}, identity, {}}};
    const auto result = ops::RunOperationElevationForTest(executable, ops::EncodeElevatedRecycle(request));
    ipc::PayloadReader reader(result.response.data(), result.response.size());
    ipc::ShellOperationResult completed;
    const bool checked = ipc::ReadShellOperationResult(reader, completed) && !reader.remaining() &&
        result.error == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && completed.items.size() == 2 &&
        SUCCEEDED(completed.items[0].hr) && !completed.items[0].destination.empty() &&
        !fs::exists(first) && fs::exists(stale);
    if (!checked) std::printf("[DIAG] worker hr=0x%08lX response=%zu unknown=%d\n",
        static_cast<unsigned long>(result.error), result.response.size(), result.outcome_unknown);
    std::printf("[%s] same-privilege recycle worker preserves a completed item and rejects a stale next item; this does not prove UAC\n",
        checked ? "PASS" : "FAIL");
    bool restored = false;
    if (checked && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        request.kind = ipc::ShellOperationKind::Move;
        request.items = {{completed.items[0].destination, first.wstring(), {}, {}}};
        const auto restored_result = shell::ExecuteFileOperation(request);
        std::string first_bytes, stale_bytes;
        std::getline(std::ifstream(first), first_bytes);
        std::getline(std::ifstream(stale), stale_bytes);
        restored = SUCCEEDED(restored_result.hr) && first_bytes == "first worker bytes" && stale_bytes == "stale worker bytes";
        CoUninitialize();
    }
    std::printf("[%s] restore only the worker's proven recycle payload\n", restored ? "PASS" : "FAIL");
    if (checked && restored && root.parent_path() == fs::absolute(L"bench_data")) {
        fs::remove(first); fs::remove(stale); fs::remove(root);
    }
    return checked && restored ? 0 : 1;
}
