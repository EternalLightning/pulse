// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "ui_compositor.h"
#include "preview_viewport.h"
#include "../ipc/preview_protocol.h"
#include <array>
#include <atomic>
#include <memory>
#include <functional>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace pulse::ui {
enum class PreviewDrawResult { Pending, Bitmap, Text, Hex, Failed };

struct PreviewProperty {
    std::wstring label;
    std::wstring value;
};

class ThumbnailCache {
public:
    ThumbnailCache();
    ~ThumbnailCache();
    enum class Consumer : size_t { Main, QuickPreview };
    class FrameScope {
    public:
        explicit FrameScope(ThumbnailCache& cache, Consumer consumer = Consumer::Main)
            : cache_(cache), consumer_(consumer) { cache_.BeginFrame(consumer_); }
        ~FrameScope() { cache_.EndFrame(consumer_); }
        FrameScope(const FrameScope&) = delete;
        FrameScope& operator=(const FrameScope&) = delete;
    private:
        ThumbnailCache& cache_;
        Consumer consumer_;
    };
    void SetDeviceContext(ID2D1DeviceContext* dc, Consumer consumer = Consumer::Main);
    void SetNotifyWindow(HWND hwnd, Consumer consumer = Consumer::Main);
    // Reset is service-wide shutdown; Evict only cancels the named consumer.
    void Reset();
    void Warm();
    // A replayed row still belongs to the current visible thumbnail working set.
    void RetainThumbnail(const std::wstring& path, DWORD attrs, uint32_t pixels,
                         uint64_t modified, uint64_t size);
    bool Prefetch(const std::wstring& path, DWORD attrs, uint32_t pixels,
                  uint64_t generation, uint64_t modified, uint64_t size);
    void Evict(Consumer consumer = Consumer::Main);
    uint64_t ContentRevision() const { return content_revision_.load(std::memory_order_relaxed); }
    // pan_x/pan_y non-null: cover mode (fill dest, crop overflow) with a
    // draggable pan offset in DIPs; values are clamped and written back.
    // pan_max_x/pan_max_y (optional) receive the current pan limits.
    PreviewDrawResult Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                           const std::wstring& path, DWORD attrs, uint32_t pixel_size,
                           uint64_t generation, uint64_t modified, uint64_t size,
                           float opacity = 1.0f, std::wstring* text = nullptr,
                           bool* truncated = nullptr, uint32_t* bytes_read = nullptr,
                           bool direct_preview = false, std::wstring* error = nullptr,
                           float* pan_x = nullptr, float* pan_y = nullptr,
                           float* pan_max_x = nullptr, float* pan_max_y = nullptr,
                           uint32_t frame_index = 0, uint32_t* frame_count = nullptr,
                           uint32_t* frame_delay_ms = nullptr, uint32_t* loop_count = nullptr,
                           uint32_t* decoded_width = nullptr, uint32_t* decoded_height = nullptr,
                           uint32_t* source_width = nullptr, uint32_t* source_height = nullptr,
                           PreviewViewport* viewport = nullptr,
                           Consumer consumer = Consumer::Main);
    bool Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                    uint64_t modified, uint64_t size,
                    std::vector<PreviewProperty>& properties,
                    Consumer consumer = Consumer::Main);
    bool CachedProperties(const std::wstring& path, uint64_t modified, uint64_t size,
                          std::vector<PreviewProperty>& properties);
private:
    friend struct ThumbnailCacheTestAccess;
    friend struct ThumbnailPipelineTestAccess;
    struct Item {
        std::array<ComPtr<ID2D1Bitmap>, 2> bitmaps;
        std::vector<uint8_t> pixels;
        std::wstring text;
        std::wstring error;
        std::vector<PreviewProperty> properties;
        uint32_t w = 0, h = 0, stride = 0;
        uint32_t bytes_read = 0;
        uint32_t frame_count = 1;
        uint32_t frame_delay_ms = 0;
        uint32_t loop_count = 0;
        uint32_t frame_index = 0;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        std::wstring animation_identity;
        ipc::PreviewContentKind kind = ipc::PreviewContentKind::None;
        bool truncated = false;
        bool failed = false;
        bool retryable = false;
        uint32_t failure_attempts = 0;
        size_t cost = 0;
        std::list<std::wstring>::iterator lru_position;
    };
    struct Request {
        uint32_t id = 0;
        uint64_t generation = 0;
        uint32_t pixels = 0;
        DWORD attrs = 0;
        ipc::PreviewRequestKind kind = ipc::PreviewRequestKind::Content;
        uint32_t frame_index = 0;
        uint32_t epoch = 0;
        uint32_t failure_attempts = 0;
        ULONGLONG retry_due = 0;
        bool details = false;
        bool prefetch = false;
        uint64_t modified = 0, size = 0;
        std::wstring path, key, identity;
    };
    struct Interest {
        bool active = false;
        bool details = false;
        uint64_t generation = 0;
        uint32_t epoch = 0;
    };
    struct Pending {
        uint32_t id = 0;
        std::wstring identity;
        std::array<Interest, 2> interests;
    };
    struct ConsumerState {
        ID2D1DeviceContext* dc = nullptr;
        HWND hwnd = nullptr;
        uint32_t epoch = 1;
        uint64_t details_generation = 0;
        std::wstring details_identity;
        bool painting = false;
        std::unordered_set<std::wstring> visible_keys;
        std::unordered_set<std::wstring> frame_keys;
    };
    void BeginFrame(Consumer consumer);
    void EndFrame(Consumer consumer);
    void KeepVisible(Consumer consumer, const std::wstring& key);
    void SelectDetails(Consumer consumer, const std::wstring& identity, uint64_t generation);
    void CancelInterests(Consumer consumer, bool details_only);
    void QueueRequest(Request request, Consumer consumer);
    void Trim();
    void StartWorkers();
    std::deque<Request>::iterator NextRequest(size_t lane, ULONGLONG now);
    void Worker(size_t lane = 0);
    bool StoreResult(const Request& request, Item result);
    void Touch(Item& item);
    bool Connect(size_t lane = 0);
    bool FetchResponse(const Request& request, Item& result, size_t lane = 0);
    std::function<void()> before_response_body_; // regression gate, empty in production
    void StopChild(size_t lane = 0);
    std::wstring Key(const std::wstring& path, uint32_t pixels, uint64_t modified,
                     uint64_t size, uint32_t frame_index = 0) const;
    std::array<ConsumerState, 2> consumers_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stop_event_ = nullptr; // persistent until the worker has joined
    PROCESS_INFORMATION child_{};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
    uint32_t pipe_token_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    std::unordered_map<std::wstring, Pending> pending_;
    std::unordered_map<std::wstring, Item> items_;
    std::list<std::wstring> lru_;
    size_t cache_bytes_ = 0;
    std::atomic<uint32_t> epoch_{1};
    std::atomic<uint64_t> content_revision_{1};
    std::thread worker_;
    struct Host {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION child{};
        uint32_t token = 0;
        std::thread worker;
    };
    std::array<Host, 3> hosts_;
    size_t lane_count_ = 1;
};
} // namespace pulse::ui
