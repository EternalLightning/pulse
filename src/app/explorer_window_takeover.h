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
};
class ExplorerWindowTakeover {
public:
    struct Shared;
    ExplorerWindowTakeover();
    ~ExplorerWindowTakeover();
    ExplorerWindowTakeover(const ExplorerWindowTakeover&) = delete;
    ExplorerWindowTakeover& operator=(const ExplorerWindowTakeover&) = delete;
    bool Start(HWND window, UINT wake_message);
    bool IsRequestActive(uint64_t token) const;
    std::vector<ExplorerTakeoverRequest> DrainRequests();
    // Call only after navigation completed and all requested selection was applied.
    // Failure, cancellation, stale completion, or lack of acknowledgment leaves Explorer open.
    void Acknowledge(uint64_t token, bool delivered);
    HRESULT LastError() const;
    void Stop();
private:
    mutable std::mutex lifecycle_;
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
    uint64_t next_token_ = 1;
};
} // namespace pulse::app
