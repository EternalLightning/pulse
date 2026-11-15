// SPDX-License-Identifier: Apache-2.0
#include "../ops/ops_manager.h"
#include <cstdio>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Seed(OpsManager& manager) {
        manager.running_ = manager.accepting_ = true;
        manager.elevation_retry_.reset(); manager.lock_retry_.reset(); manager.queue_.clear();
        manager.shell_elevation_authorized_ = false;
        manager.SetStatus([](OpStatus& status) {
            status = {}; status.task_id = 17; status.phase = OpPhase::Failed;
        });
    }
    static bool Capture(OpsManager& manager, OpType type, HRESULT error, bool already_consented = false,
        bool remote = false) {
        Seed(manager);
        OpsManager::QueueItem retry;
        retry.seq = 17; retry.req.type = type;
        retry.req.sources = {remote ? L"\\\\server\\share\\file.txt" : L"E:\\isolated-permission-fixture\\file.txt"};
        retry.req.dest_dir = L"E:\\isolated-permission-output";
        retry.elevated_retry = already_consented;
        const bool accepted = manager.CaptureElevationFailure(retry, error, retry.req.dest_dir);
        manager.SetStatus([](OpStatus&) {});
        return accepted;
    }
    static bool Consented(const OpsManager& manager) {
        return manager.queue_.size() == 1 && manager.queue_.front().elevated_retry &&
            !manager.queue_.front().lock_retry && manager.queue_.front().seq == 17;
    }
    static bool Empty(const OpsManager& manager) { return manager.queue_.empty(); }
};
}

int main() {
    using namespace pulse::ops;
    OpsManager manager;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    for (const auto type : {OpType::Copy, OpType::Move, OpType::Rename, OpType::BatchRename,
        OpType::CreateFolder, OpType::CreateTextFile, OpType::RecycleDelete, OpType::RealDelete}) {
        check(TransferIntegrationProbe::Capture(manager, type, E_ACCESSDENIED) &&
            manager.IsElevationFailureCurrent(17) && TransferIntegrationProbe::Empty(manager),
            "local permission failure offers Pulse confirmation without launching an administrator operation");
        manager.CancelElevationRetry(17);
        check(!manager.IsElevationFailureCurrent(17) && TransferIntegrationProbe::Empty(manager),
            "declining Pulse confirmation launches nothing");
    }
    for (const auto error : {S_OK, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION),
        HRESULT_FROM_WIN32(ERROR_WRITE_PROTECT), HRESULT_FROM_WIN32(ERROR_FILE_INVALID),
        HRESULT_FROM_WIN32(ERROR_CANCELLED), HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)}) {
        check(!TransferIntegrationProbe::Capture(manager, OpType::Copy, error) &&
            !manager.IsElevationFailureCurrent(17),
            "success, locks, write protection, stale identity, cancellation and missing paths never imply elevation");
    }
    check(!TransferIntegrationProbe::Capture(manager, OpType::Copy, E_ACCESSDENIED, true),
        "a failed administrator retry never prompts for administrator permission again");
    check(!TransferIntegrationProbe::Capture(manager, OpType::Copy, E_ACCESSDENIED, false, true),
        "remote access denial never offers local administrator elevation");
    TransferIntegrationProbe::Capture(manager, OpType::Copy, E_ACCESSDENIED);
    check(!manager.RetryElevatedOperation(18) && TransferIntegrationProbe::Empty(manager),
        "stale task consent cannot authorize another request");
    check(manager.RetryElevatedOperation(17) && TransferIntegrationProbe::Consented(manager) &&
        !manager.RetryElevatedOperation(17), "explicit current consent queues exactly one authorized retry");
    manager.Stop();
    return failures ? 1 : 0;
}
