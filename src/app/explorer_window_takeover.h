// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {
struct ExplorerTakeoverRequest {
    uint64_t token = 0;
    std::wstring folder; // Empty denotes This PC.
    std::vector<std::wstring> selected_paths;
    uint64_t replaces_token = 0;
};
struct ExplorerTakeoverStats {
    uint64_t scans = 0;
    uint64_t submitted = 0;
    uint64_t replaced = 0;
    uint64_t cancelled = 0;
    uint64_t close_attempts = 0;
    uint64_t close_failures = 0;
};
class ExplorerWindowTakeover {
public:
    struct Shared;
    ExplorerWindowTakeover();
    ~ExplorerWindowTakeover();
    ExplorerWindowTakeover(const ExplorerWindowTakeover&) = delete;
    ExplorerWindowTakeover& operator=(const ExplorerWindowTakeover&) = delete;
    bool Start(HWND window, UINT wake_message);
    // Both allowlists are mandatory; existing exact source identities may be
    // exercised without watching or closing any other Explorer window.
    bool StartForTesting(HWND window, UINT wake_message,
                         const std::vector<HWND>& allowed_windows,
                         const std::vector<std::wstring>& allowed_folders);
    ExplorerTakeoverStats Stats() const;
    bool IsRequestActive(uint64_t token) const;
    std::vector<ExplorerTakeoverRequest> DrainRequests();
    // Call only after navigation completed and all requested selection was applied.
    // Failure, cancellation, stale completion, or lack of acknowledgment leaves Explorer open.
    void Acknowledge(uint64_t token, bool delivered);
    HRESULT LastError() const;
    // Cancels uncommitted closes. A previously committed COM Quit may still be
    // in flight; shutdown never terminates the STA or changes source visibility.
    void Stop();
private:
    bool StartImpl(HWND window, UINT wake_message, const std::vector<HWND>& allowed_windows,
                   const std::vector<std::wstring>& allowed_folders, bool scoped);
    mutable std::mutex lifecycle_;
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
    uint64_t next_token_ = 1;
};
} // namespace pulse::app
