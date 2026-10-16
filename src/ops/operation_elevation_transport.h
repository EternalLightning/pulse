#pragma once

#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulse::ops {

inline constexpr size_t kOperationElevationMaxPayload = 8u * 1024u * 1024u;

struct OperationElevationResult {
    HRESULT error = E_FAIL;
    bool cancelled = false;
    // A request was submitted, but no authenticated terminal result arrived.
    bool outcome_unknown = false;
    std::vector<uint8_t> response;
};

struct OperationElevationCallbacks {
    void* context = nullptr;
    std::vector<uint8_t> (*on_progress)(void*, std::span<const uint8_t>) = nullptr;
    // Invoked from the operation thread and a cancellation monitor: thread-safe.
    bool (*cancelled)(void*) = nullptr;
};

// on_progress returns the control acknowledgement (empty for normal progress).
// Blocking: call on the operation thread, never the UI thread. Payload codecs
// must validate operation kinds, paths, identities and cumulative snapshots.
OperationElevationResult RunElevatedOperation(const std::wstring& helper_exe,
    HWND owner, std::span<const uint8_t> request,
    const OperationElevationCallbacks& callbacks = {});

class OperationElevationWorkerChannel {
public:
    bool ReportProgress(std::span<const uint8_t> snapshot, std::vector<uint8_t>* control = nullptr);
    bool IsCancelled() const;
private:
    explicit OperationElevationWorkerChannel(void* state) : state_(state) {}
    void* state_ = nullptr;
    friend int RunOperationElevationWorker(int, wchar_t**, const struct OperationElevationExecutor&);
};

struct OperationElevationExecutor {
    void* context = nullptr;
    OperationElevationResult (*execute)(void*, std::span<const uint8_t>,
        OperationElevationWorkerChannel&) = nullptr;
};

// argv: executable, --file-operation-worker, pipe, parent PID, creation time.
// Returns ERROR_INVALID_PARAMETER when argv is not this worker entry point.
int RunOperationElevationWorker(int argc, wchar_t** argv,
    const OperationElevationExecutor& executor);

#ifdef PULSE_OPERATION_ELEVATION_TESTING
// This target uses CreateProcess at the caller's integrity level; it never
// proves that UAC consent or elevated filesystem access works.
OperationElevationResult RunOperationElevationForTest(const std::wstring& helper_exe,
    std::span<const uint8_t> request,
    const OperationElevationCallbacks& callbacks = {});
bool OperationElevationPathsForTest(const std::wstring& parent, const std::wstring& worker, bool testing);
#endif

} // namespace pulse::ops
