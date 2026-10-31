// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pulse::ops {

// Copy workers own only their copy state. Admission, pathname changes and
// deletion remain on the transfer thread after the worker has returned.
class TransferWorkQueue {
public:
    explicit TransferWorkQueue(unsigned count) {
        try {
            for (unsigned i = 0; i < count; ++i) threads_.emplace_back([this] {
                for (;;) {
                    std::function<void()> work;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [&] { return closing_ || !queue_.empty(); });
                        if (queue_.empty()) return;
                        work = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    work();
                }
            });
        } catch (...) {
            Close();
            throw;
        }
    }
    ~TransferWorkQueue() { Close(); }
    TransferWorkQueue(const TransferWorkQueue&) = delete;
    TransferWorkQueue& operator=(const TransferWorkQueue&) = delete;

    void Submit(std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(work));
        }
        cv_.notify_one();
    }
    std::atomic<bool>& CancelFlag() { return cancelled_; }
    void Close() {
        cancelled_.store(true);
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
        }
        cv_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
        threads_.clear();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    bool closing_ = false;
    std::atomic<bool> cancelled_{false};
};

} // namespace pulse::ops
