// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace pulse::ui {
class RepaintTargets {
public:
    void Set(HWND hwnd) {
        std::lock_guard lock(mutex_);
        windows_.clear();
        if (hwnd) windows_.push_back(hwnd);
    }
    void Add(HWND hwnd) {
        if (!hwnd) return;
        std::lock_guard lock(mutex_);
        if (std::find(windows_.begin(), windows_.end(), hwnd) == windows_.end()) windows_.push_back(hwnd);
    }
    void Remove(HWND hwnd) {
        std::lock_guard lock(mutex_);
        std::erase(windows_, hwnd);
    }
    size_t Count() const {
        std::lock_guard lock(mutex_);
        return windows_.size();
    }
    void InvalidateAll() const {
        revision_.fetch_add(1, std::memory_order_relaxed);
        // InvalidateRect only marks the update region, so workers never call
        // into a window while holding this gate. Remove serializes teardown.
        std::lock_guard lock(mutex_);
        for (HWND hwnd : windows_) InvalidateRect(hwnd, nullptr, FALSE);
    }
    uint64_t Revision() const { return revision_.load(std::memory_order_relaxed); }
private:
    mutable std::atomic<uint64_t> revision_{1};
    mutable std::mutex mutex_;
    std::vector<HWND> windows_;
};
}
