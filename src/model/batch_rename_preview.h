#pragma once
#include "batch_rename.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace pulse::model {

// A single worker keeps only the newest request. A provider call already in
// progress may finish after Close; it never holds a window or dialog pointer.
class BatchRenamePreview {
public:
    using Exists = std::function<bool(const std::wstring&)>;
    explicit BatchRenamePreview(Exists exists = {}) : state_(std::make_shared<State>()) {
        state_->exists = std::move(exists);
        std::thread([state = state_] { Run(state); }).detach();
    }
    ~BatchRenamePreview() { Close(); }
    BatchRenamePreview(const BatchRenamePreview&) = delete;
    BatchRenamePreview& operator=(const BatchRenamePreview&) = delete;

    void Request(std::vector<std::wstring> paths, BatchRenameRule rule) {
        std::lock_guard lock(state_->mutex);
        if (state_->closed) return;
        const auto generation = ++state_->generation;
        state_->result.reset();
        state_->pending = RequestData{std::move(paths), std::move(rule), generation};
        state_->ready.notify_one();
    }
    std::optional<std::vector<BatchRenameItem>> Take() {
        std::lock_guard lock(state_->mutex);
        auto result = std::move(state_->result);
        state_->result.reset();
        return result;
    }
    void Close() {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        ++state_->generation;
        state_->pending.reset();
        state_->result.reset();
        state_->ready.notify_one();
    }

private:
    struct RequestData {
        std::vector<std::wstring> paths;
        BatchRenameRule rule;
        uint64_t generation;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable ready;
        std::atomic<uint64_t> generation{0};
        bool closed = false;
        std::optional<RequestData> pending;
        std::optional<std::vector<BatchRenameItem>> result;
        Exists exists;
    };
    static void Run(const std::shared_ptr<State>& state) {
        for (;;) {
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return state->closed || state->pending.has_value(); });
            if (state->closed) return;
            auto request = std::move(*state->pending);
            state->pending.reset();
            lock.unlock();
            std::vector<BatchRenameItem> items;
            try {
                items = PreviewBatchRename(request.paths, request.rule,
                    [&] { return state->generation != request.generation; }, state->exists);
            } catch (...) {
                // A failed preview remains unavailable; no partial plan is accepted.
                items.clear();
            }
            lock.lock();
            if (!state->closed && state->generation == request.generation)
                state->result = std::move(items);
        }
    }
    std::shared_ptr<State> state_;
};

} // namespace pulse::model
