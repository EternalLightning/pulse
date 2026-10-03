#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {
struct ShellWindowEntry {
    uint64_t key = 0; // Stable tab/pane identity, never a recycled pointer.
    std::wstring path; // Empty denotes This PC; omit other virtual pages.
};
struct ShellSelectRequest {
    uint64_t key = 0;
    std::wstring folder;
    std::wstring path;
    unsigned flags = 0;
};
bool ShellSelectionFlagsNeedDelivery(unsigned flags);
class ShellWindowRegistry {
public:
    struct Shared;
    ShellWindowRegistry();
    ~ShellWindowRegistry();
    ShellWindowRegistry(const ShellWindowRegistry&) = delete;
    ShellWindowRegistry& operator=(const ShellWindowRegistry&) = delete;
    // Startup is asynchronous; publications are retained until the STA is ready.
    bool Start(HWND window, UINT wake_message);
    void Publish(std::vector<ShellWindowEntry> entries);
    std::vector<ShellSelectRequest> DrainSelections();
    HRESULT LastError() const;
    // Stop before DestroyWindow, preferably in the existing shutdown worker.
    void Stop();
private:
    mutable std::mutex lifecycle_;
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};
} // namespace pulse::app
