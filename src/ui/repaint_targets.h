#pragma once
#include <windows.h>
#include <algorithm>
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
        // InvalidateRect only marks the update region, so workers never call
        // into a window while holding this gate. Remove serializes teardown.
        std::lock_guard lock(mutex_);
        for (HWND hwnd : windows_) InvalidateRect(hwnd, nullptr, FALSE);
    }
private:
    mutable std::mutex mutex_;
    std::vector<HWND> windows_;
};
}
