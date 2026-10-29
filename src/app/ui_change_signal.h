#pragma once
#include <windows.h>
#include <mutex>

namespace pulse::app {

// Coalesces background results until the UI consumes them. Disconnect and
// posting share the lock, so a stopped producer cannot post to a reused HWND.
class UiChangeSignal {
public:
    void SetTarget(HWND hwnd, UINT message) {
        std::lock_guard lock(mutex_);
        hwnd_ = hwnd;
        message_ = message;
        if (changed_ && hwnd_ && message_) PostMessageW(hwnd_, message_, 0, 0);
    }
    UiChangeSignal& operator=(bool changed) {
        std::lock_guard lock(mutex_);
        if (changed && !changed_ && hwnd_ && message_) PostMessageW(hwnd_, message_, 0, 0);
        changed_ = changed;
        return *this;
    }
    bool Take() {
        std::lock_guard lock(mutex_);
        const bool changed = changed_;
        changed_ = false;
        return changed;
    }
private:
    std::mutex mutex_;
    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    bool changed_ = false;
};

} // namespace pulse::app
