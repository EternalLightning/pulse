#pragma once

#include "ops_manager.h"
#include "operation_elevation_transport.h"

namespace pulse::ops {

// The queue item and its source leases remain alive in the parent until the
// authenticated worker returns. This adapter never automatically replays a
// request whose terminal outcome was lost.
class OperationElevationAdapter {
public:
    static void RunParent(OpsManager& manager, const OpsManager::QueueItem& item);
    static int RunWorker(int argc, wchar_t** argv);
#ifdef PULSE_OPERATION_ELEVATION_TESTING
    static void RunFixtureForTest(OpsManager&, const OpRequest&, const std::wstring& helper,
                                  bool native_consent = false,
                                  const std::function<void()>& before_launch = {});
    static bool PartialCopyContinuationForTest(OpsManager&, const OpRequest&, const std::wstring& helper);
    static bool CopyUndoHistoryValidForTest(OpsManager&);
    static bool RunUndoFixtureForTest(OpsManager&, const std::wstring& helper);
    static bool UnknownRecoveryPersistenceForTest(const std::wstring& journal_path);
    static bool CodecChecksForTest();
#endif
private:
    static void RunParentImpl(OpsManager&, const OpsManager::QueueItem&, const std::wstring&, bool);
    struct ParentState;
    struct WorkerState;
    static std::vector<uint8_t> ParentProgress(void*, std::span<const uint8_t>);
    static bool ParentCancelled(void*);
    static OperationElevationResult ExecuteWorker(void*, std::span<const uint8_t>,
                                                  OperationElevationWorkerChannel&);
    friend struct OperationElevationAdapterTestProbe;
};

} // namespace pulse::ops
