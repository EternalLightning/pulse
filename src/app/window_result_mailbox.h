// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace pulse::app {

// Workers retain only this mailbox. Closing and posting share a lock, so no
// producer can post after Close returns. Messages carry identity, never ownership.
template<class T>
class WindowResultMailbox {
public:
    WindowResultMailbox(HWND hwnd, UINT message)
        : hwnd_(hwnd), message_(message), identity_(next_identity_.fetch_add(1)) {}

    bool Publish(T result) {
        std::lock_guard lock(mutex_);
        if (!hwnd_) return false;
        results_.push_back(std::move(result));
        if (PostMessageW(hwnd_, message_, identity_, 0)) return true;
        results_.pop_back();
        return false;
    }

    std::vector<T> Take(WPARAM identity) {
        std::lock_guard lock(mutex_);
        std::vector<T> results;
        if (hwnd_ && identity == identity_) results.swap(results_);
        return results;
    }

    void Close() {
        std::lock_guard lock(mutex_);
        hwnd_ = nullptr;
        results_.clear();
    }

private:
    inline static std::atomic<WPARAM> next_identity_{1};
    std::mutex mutex_;
    HWND hwnd_;
    const UINT message_;
    const WPARAM identity_;
    std::vector<T> results_;
};

} // namespace pulse::app
