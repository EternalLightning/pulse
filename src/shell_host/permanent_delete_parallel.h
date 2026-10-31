// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../fs/drive_kind.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace pulse::shell::detail {

inline bool PermanentDeletesOverlapOn(const std::wstring& name) {
    const auto plain = path::StripExtendedPathPrefix(name);
    return plain.starts_with(L"\\\\") || fs::IsRemoteLikeDrive(plain);
}

// Only the coordinator invokes user callbacks. Workers see an atomic cancel
// flag, and completion is published before its result is read by the caller.
template <typename Work, typename Complete>
void RunPermanentDeleteBatch(size_t count, const std::function<bool()>& cancelled,
    Work&& work, Complete&& complete) {
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false}, cancel{false};
    std::vector<std::atomic<bool>> done(count);
    std::vector<bool> reported(count, false);
    const std::function<bool()> worker_cancelled = [&] { return cancel.load(); };
    const auto tick = [&] {
        if (cancelled && cancelled()) { cancel.store(true); stop.store(true); }
        for (size_t i = 0; i < count; ++i) {
            if (!reported[i] && done[i].load(std::memory_order_acquire)) {
                reported[i] = true;
                complete(i);
            }
        }
        if (cancelled && cancelled()) { cancel.store(true); stop.store(true); }
    };
    const auto drain = [&] {
        while (!stop.load()) {
            const size_t i = next.fetch_add(1);
            if (i >= count) break;
            const bool success = work(i, worker_cancelled);
            done[i].store(true, std::memory_order_release);
            if (!success) stop.store(true);
        }
    };
    std::mutex mutex;
    std::condition_variable finished;
    size_t running = 0;
    std::vector<std::jthread> workers;
    workers.reserve((std::min)(size_t{8}, count));
    tick();
    for (size_t i = 0; i < (std::min)(size_t{8}, count) && !stop.load(); ++i) {
        { std::lock_guard lock(mutex); ++running; }
        try {
            workers.emplace_back([&] {
                drain();
                std::lock_guard lock(mutex);
                --running;
                finished.notify_one();
            });
        } catch (const std::system_error&) {
            std::lock_guard lock(mutex);
            --running;
            break;
        }
    }
    if (workers.empty()) {
        // Resource exhaustion keeps the same authorization and serial behavior.
        for (size_t i = 0; i < count && !stop.load(); ++i) {
            tick();
            if (stop.load()) break;
            if (!work(i, worker_cancelled)) stop.store(true);
            done[i].store(true, std::memory_order_release);
            tick();
        }
        return;
    }
    {
        std::unique_lock lock(mutex);
        while (running) {
            finished.wait_for(lock, std::chrono::milliseconds(10));
            lock.unlock();
            tick();
            lock.lock();
        }
    }
    for (auto& worker : workers) worker.join();
    tick();
}

} // namespace pulse::shell::detail
