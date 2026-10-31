// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace pulse::app {
enum class FolderSizeState { Manual, Calculating, Ready, Updating, Partial, Unavailable, Cached, Indexed };
struct FolderSizeValue {
    FolderSizeState state = FolderSizeState::Manual;
    uint64_t bytes = 0;
    bool has_value = false;
    uint64_t files = 0, dirs = 0;
    bool counts_valid = false;
    bool incomplete = false;
};
struct FolderSizeRequest {
    std::wstring path;
    bool automatic = true;
    bool require_counts = false;
    bool operator==(const FolderSizeRequest&) const = default;
};

// Window-thread methods only exchange in-memory state. Enumeration, watches and
// cache I/O belong to the background worker, including network paths.
class FolderSizes {
public:
    FolderSizes();
    ~FolderSizes();
    FolderSizes(const FolderSizes&) = delete;
    FolderSizes& operator=(const FolderSizes&) = delete;
    void SetCachePath(std::function<std::wstring()> path);
    void SetIndexEnabled(bool enabled);
    void Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> watch_roots);
    bool NeedsWorker() const;
    void Calculate(const std::wstring& path);
    FolderSizeValue Get(const std::wstring& path) const;
    void Invalidate(const std::wstring& path);
    bool TakeChanged();
    void SetChangeNotify(HWND hwnd, UINT message);
    void Stop();
#ifdef PULSE_FOLDER_SIZES_TESTING
    void SetScanHookForTest(std::function<void(const std::wstring&)> hook);
    static unsigned LiveWorkersForTest();
    unsigned SyncRevisionForTest() const;
#endif
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
