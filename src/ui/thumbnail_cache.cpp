#include "thumbnail_cache.h"
#include "thumbnail_disk_cache.h"
#include "../fs/cloud_state.h"
#include "../ipc/preview_response.h"
#include <algorithm>
#include <chrono>

namespace pulse::ui {
namespace {
std::atomic<uint32_t> g_preview_cache_sequence{1};
constexpr uint32_t kMaxFailureAttempts = 3;
constexpr ULONGLONG kFailureRetryDelayMs = 2000;
}

ThumbnailCache::ThumbnailCache() {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const uint32_t sequence = g_preview_cache_sequence.fetch_add(1);
    pipe_token_ = GetCurrentProcessId() ^ (sequence * 0x9E3779B9u);
    if (!pipe_token_) pipe_token_ = sequence ? sequence : 1;
    const auto cores = std::thread::hardware_concurrency();
    lane_count_ = std::clamp<size_t>(cores > 2 ? cores - 2 : 1, 1, 4);
    for (auto& host : hosts_) {
        const uint32_t next = g_preview_cache_sequence.fetch_add(1);
        host.token = GetCurrentProcessId() ^ (next * 0x9E3779B9u);
        if (!host.token) host.token = next ? next : 1;
    }
}

ThumbnailCache::~ThumbnailCache() { Reset(); if (stop_event_) CloseHandle(stop_event_); }
void ThumbnailCache::BeginFrame(Consumer consumer) {
    std::lock_guard lock(mutex_);
    auto& state = consumers_[static_cast<size_t>(consumer)];
    state.frame_keys.clear();
    state.painting = true;
}
void ThumbnailCache::EndFrame(Consumer consumer) {
    std::lock_guard lock(mutex_);
    auto& state = consumers_[static_cast<size_t>(consumer)];
    state.visible_keys.swap(state.frame_keys);
    state.frame_keys.clear();
    state.painting = false;
    Trim();
}
void ThumbnailCache::KeepVisible(Consumer consumer, const std::wstring& key) {
    auto& state = consumers_[static_cast<size_t>(consumer)];
    if (state.painting) state.frame_keys.insert(key);
}
void ThumbnailCache::SetDeviceContext(ID2D1DeviceContext* dc, Consumer consumer) {
    std::lock_guard lock(mutex_);
    const size_t index = static_cast<size_t>(consumer);
    auto& state = consumers_[index];
    if (state.dc == dc) return;
    state.dc = dc;
    for (auto& [_, item] : items_) {
        if (item.bitmaps[index].get()) {
            const size_t bytes = static_cast<size_t>(item.stride) * item.h;
            item.cost -= bytes;
            cache_bytes_ -= bytes;
            item.bitmaps[index].reset();
        }
    }
}
void ThumbnailCache::SetNotifyWindow(HWND hwnd, Consumer consumer) {
    std::lock_guard lock(mutex_);
    consumers_[static_cast<size_t>(consumer)].hwnd = hwnd;
}
std::wstring ThumbnailCache::Key(const std::wstring& path, uint32_t pixels,
                                 uint64_t modified, uint64_t size,
                                 uint32_t frame_index) const {
    return path + L"\n" + std::to_wstring(pixels) + L":" + std::to_wstring(modified) +
           L":" + std::to_wstring(size) + L":" + std::to_wstring(frame_index);
}
void ThumbnailCache::StopChild(size_t lane) {
    auto& lane_pipe = lane ? hosts_[lane - 1].pipe : this->pipe_;
    auto& lane_child = lane ? hosts_[lane - 1].child : this->child_;
    if (lane_pipe != INVALID_HANDLE_VALUE) { CloseHandle(lane_pipe); lane_pipe = INVALID_HANDLE_VALUE; }
    if (lane_child.hProcess) { TerminateProcess(lane_child.hProcess, 0); CloseHandle(lane_child.hProcess); CloseHandle(lane_child.hThread); lane_child = {}; }
}
void ThumbnailCache::Reset() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    cv_.notify_all();
    if (stop_event_) SetEvent(stop_event_);
    if (worker_.joinable()) worker_.join();
    for (auto& host : hosts_) if (host.worker.joinable()) host.worker.join();
    for (size_t lane = 0; lane < lane_count_; ++lane) StopChild(lane);
    std::lock_guard lock(mutex_); queue_.clear(); pending_.clear(); items_.clear(); lru_.clear();
    cache_bytes_ = 0;
    for (auto& state : consumers_) {
        state.dc = nullptr; state.hwnd = nullptr;
        state.details_identity.clear(); state.details_generation = 0; ++state.epoch;
        state.visible_keys.clear(); state.frame_keys.clear(); state.painting = false;
    }
    epoch_.fetch_add(1, std::memory_order_relaxed);
}
void ThumbnailCache::CancelInterests(Consumer consumer, bool details_only) {
    const size_t index = static_cast<size_t>(consumer);
    for (auto pending = pending_.begin(); pending != pending_.end();) {
        auto& interest = pending->second.interests[index];
        if (!details_only || interest.details) interest.active = false;
        const bool live = std::any_of(pending->second.interests.begin(),
            pending->second.interests.end(), [](const Interest& i) { return i.active; });
        if (!live) {
            const uint32_t id = pending->second.id;
            std::erase_if(queue_, [id](const Request& req) { return req.id == id; });
            pending = pending_.erase(pending);
        } else ++pending;
    }
}
void ThumbnailCache::Evict(Consumer consumer) {
    std::lock_guard lock(mutex_);
    auto& state = consumers_[static_cast<size_t>(consumer)];
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second.failed && it->second.retryable &&
            (state.visible_keys.contains(it->first) || state.frame_keys.contains(it->first))) {
            cache_bytes_ -= it->second.cost;
            lru_.erase(it->second.lru_position);
            it = items_.erase(it);
        } else ++it;
    }
    ++state.epoch;
    state.details_identity.clear();
    state.visible_keys.clear(); state.frame_keys.clear(); state.painting = false;
    CancelInterests(consumer, false);
    Trim();
}
void ThumbnailCache::SelectDetails(Consumer consumer, const std::wstring& identity,
                                   uint64_t generation) {
    auto& state = consumers_[static_cast<size_t>(consumer)];
    if (state.details_identity == identity && state.details_generation == generation) return;
    CancelInterests(consumer, true);
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->second.failed && it->second.retryable &&
            it->second.animation_identity == identity) {
            cache_bytes_ -= it->second.cost;
            lru_.erase(it->second.lru_position);
            it = items_.erase(it);
        } else ++it;
    }
    state.details_identity = identity;
    state.details_generation = generation;
}
void ThumbnailCache::QueueRequest(Request request, Consumer consumer) {
    const size_t index = static_cast<size_t>(consumer);
    auto& state = consumers_[index];
    if (auto found = pending_.find(request.key); found != pending_.end()) {
        auto& interest = found->second.interests[index];
        // A list request must remain valid even when the same window changes its details selection.
        const bool details = interest.active ? interest.details && request.details : request.details;
        interest = {true, details, request.generation, state.epoch};
        if (!request.prefetch) {
            for (auto& queued : queue_) if (queued.id == found->second.id) queued.prefetch = false;
            cv_.notify_all();
        }
        return;
    }
    request.id = next_id_++;
    request.epoch = epoch_.load(std::memory_order_relaxed);
    Pending pending;
    pending.id = request.id; pending.identity = request.identity;
    pending.interests[index] = {true, request.details, request.generation, state.epoch};
    pending_.emplace(request.key, std::move(pending));
    if (request.prefetch || request.kind == ipc::PreviewRequestKind::Properties) queue_.push_back(std::move(request));
    else queue_.push_front(std::move(request));
    if (queue_.size() > 128) { pending_.erase(queue_.back().key); queue_.pop_back(); }
    StartWorkers();
    cv_.notify_all();
}
void ThumbnailCache::StartWorkers() {
    if (running_.exchange(true)) return;
    if (stop_event_) ResetEvent(stop_event_);
    size_t started = 0;
    try {
        worker_ = std::thread([this] { Worker(); });
        started = 1;
        for (size_t lane = 1; lane < lane_count_; ++lane) {
            hosts_[lane - 1].worker = std::thread([this, lane] { Worker(lane); });
            ++started;
        }
    } catch (const std::system_error&) {
        // Keep the lanes already started; a later request can retry if none
        // could be created. The UI still draws its normal file icon.
        if (!started) running_ = false;
        else lane_count_ = started;
    }
}
void ThumbnailCache::Warm() {
    std::lock_guard lock(mutex_);
    StartWorkers();
}
bool ThumbnailCache::Prefetch(const std::wstring& path, DWORD attrs, uint32_t pixels,
                               uint64_t generation, uint64_t modified, uint64_t size) {
    if (path.empty() || pixels < 24 || pixels > 512 ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) || fs::IsCloudPlaceholder(attrs)) return true;
    // Restrict speculative work to cheap still-image formats.
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return true;
    const auto ext = path.substr(dot);
    if (_wcsicmp(ext.c_str(), L".jpg") && _wcsicmp(ext.c_str(), L".jpeg") &&
        _wcsicmp(ext.c_str(), L".png") && _wcsicmp(ext.c_str(), L".bmp") &&
        _wcsicmp(ext.c_str(), L".webp")) return true;
    std::lock_guard lock(mutex_);
    const auto key = Key(path, pixels, modified, size) + L":thumbnail";
    if (items_.contains(key) || pending_.contains(key)) return true;
    if (queue_.size() >= 64 || lane_count_ < 2) return false;
    Request req;
    req.path = path; req.attrs = attrs; req.pixels = pixels; req.generation = generation;
    req.modified = modified; req.size = size;
    req.kind = ipc::PreviewRequestKind::Thumbnail; req.prefetch = true;
    req.key = key; req.identity = Key(path, 0, modified, size);
    QueueRequest(std::move(req), Consumer::Main);
    return true;
}
bool ThumbnailCache::Connect(size_t lane) {
    auto& lane_pipe = lane ? hosts_[lane - 1].pipe : this->pipe_;
    auto& lane_child = lane ? hosts_[lane - 1].child : this->child_;
    const auto lane_token = lane ? hosts_[lane - 1].token : this->pipe_token_;
    if (!running_ || !stop_event_ || WaitForSingleObject(stop_event_, 0) != WAIT_TIMEOUT) return false;
    if (lane_pipe != INVALID_HANDLE_VALUE) return true;
    const std::wstring pipeName = ipc::PreviewPipeName(lane_token);
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    wchar_t* slash = wcsrchr(exe, L'\\'); if (!slash) return false; *(slash + 1) = 0;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
        std::to_wstring(lane_token);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_FORCEOFFFEEDBACK; // background helper: no AppStarting cursor
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &lane_child)) return false;
    const ULONGLONG deadline = GetTickCount64() + 3000;
    do {
        lane_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (lane_pipe != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            if (GetNamedPipeServerProcessId(lane_pipe, &server_pid) &&
                server_pid == lane_child.dwProcessId)
                return true;
            CloseHandle(lane_pipe);
            lane_pipe = INVALID_HANDLE_VALUE;
            StopChild(lane);
            return false;
        }
        Sleep(25);
    } while (running_ && GetTickCount64() < deadline);
    StopChild(lane); return false;
}
PreviewDrawResult ThumbnailCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                                       const std::wstring& path, DWORD attrs, uint32_t pixels,
                                       uint64_t generation, uint64_t modified, uint64_t size,
                                       float opacity, std::wstring* text,
                                       bool* truncated, uint32_t* bytes_read,
                                       bool direct_preview, std::wstring* error,
                                       float* pan_x, float* pan_y,
                                       float* pan_max_x, float* pan_max_y,
                                       uint32_t frame_index, uint32_t* frame_count,
                                       uint32_t* frame_delay_ms, uint32_t* loop_count,
                                       uint32_t* decoded_width, uint32_t* decoded_height,
                                       uint32_t* source_width, uint32_t* source_height,
                                       PreviewViewport* viewport, Consumer consumer) {
    if (!dc || path.empty() || pixels < 24) return PreviewDrawResult::Failed;
    if (direct_preview && fs::IsCloudPlaceholder(attrs)) {
        if (error) *error = L"offline-placeholder";
        return PreviewDrawResult::Failed;
    }
    SetDeviceContext(dc, consumer);
    const std::wstring key = Key(path, pixels, modified, size, frame_index) +
        (direct_preview ? L"" : fs::IsCloudPlaceholder(attrs) ? L":thumbnail:cloud-cache" : L":thumbnail");
    {
        std::lock_guard lock(mutex_);
        KeepVisible(consumer, key);
        const std::wstring identity = Key(path, 0, modified, size);
        if (direct_preview) SelectDetails(consumer, identity, generation);
        auto queue_request = [&] {
            Request request;
            request.generation = generation; request.pixels = pixels; request.attrs = attrs;
            request.kind = direct_preview ? ipc::PreviewRequestKind::Content : ipc::PreviewRequestKind::Thumbnail;
            request.modified = modified; request.size = size;
            request.details = direct_preview; request.frame_index = frame_index;
            request.path = path; request.key = key; request.identity = identity;
            QueueRequest(std::move(request), consumer);
        };
        auto it = items_.find(key);
        if (it != items_.end()) {
            Item& item = it->second;
            Touch(item);
            if (truncated) *truncated = item.truncated;
            if (bytes_read) *bytes_read = item.bytes_read;
            if (error) *error = item.error;
            if (frame_count) *frame_count = item.frame_count;
            if (frame_delay_ms) *frame_delay_ms = item.frame_delay_ms;
            if (loop_count) *loop_count = item.loop_count;
            if (decoded_width) *decoded_width = item.w;
            if (decoded_height) *decoded_height = item.h;
            if (source_width) *source_width = item.source_width;
            if (source_height) *source_height = item.source_height;
            if (item.kind == ipc::PreviewContentKind::Text ||
                item.kind == ipc::PreviewContentKind::Hex) {
                if (text) *text = item.text;
                return item.kind == ipc::PreviewContentKind::Hex
                    ? PreviewDrawResult::Hex : PreviewDrawResult::Text;
            }
            auto& bitmap = item.bitmaps[static_cast<size_t>(consumer)];
            if (!bitmap.get() && !item.pixels.empty()) {
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(item.w, item.h),
                        item.pixels.data(), item.stride, &props, &bitmap))) {
                    // Keep CPU pixels for the other D2D device and device-loss recovery.
                    const size_t bytes = static_cast<size_t>(item.stride) * item.h;
                    item.cost += bytes; cache_bytes_ += bytes;
                }
            }
            if (bitmap.get()) {
                if (viewport) {
                    viewport->SetContent(dest, static_cast<float>(item.source_width ? item.source_width : item.w),
                        static_cast<float>(item.source_height ? item.source_height : item.h), true);
                    const auto target = viewport->ContentRect();
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(bitmap.get(), &target, std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    Trim();
                    return PreviewDrawResult::Bitmap;
                }
                const float destW = std::max(1.0f, dest.right - dest.left);
                const float destH = std::max(1.0f, dest.bottom - dest.top);
                if (pan_x && pan_y) {
                    // Cover: fill the dest rect and crop the overflow; pan is a
                    // DIP offset into the cropped region. Bitmaps smaller than
                    // the dest stay at scale<=1 (centered, nothing to pan).
                    float cover = std::max(destW / static_cast<float>(item.w),
                                           destH / static_cast<float>(item.h));
                    cover = std::min(cover, 1.0f);
                    const float drawW = item.w * cover;
                    const float drawH = item.h * cover;
                    const float maxPanX = std::max(0.0f, drawW - destW);
                    const float maxPanY = std::max(0.0f, drawH - destH);
                    if (pan_max_x) *pan_max_x = maxPanX;
                    if (pan_max_y) *pan_max_y = maxPanY;
                    *pan_x = std::clamp(*pan_x, 0.0f, maxPanX);
                    *pan_y = std::clamp(*pan_y, 0.0f, maxPanY);
                    const D2D1_RECT_F fitted = D2D1::RectF(
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y,
                        dest.left + std::max(0.0f, destW - drawW) * 0.5f - *pan_x + drawW,
                        dest.top + std::max(0.0f, destH - drawH) * 0.5f - *pan_y + drawH);
                    dc->PushAxisAlignedClip(dest, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    dc->DrawBitmap(bitmap.get(), &fitted,
                        std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->PopAxisAlignedClip();
                    Trim();
                    return PreviewDrawResult::Bitmap;
                }
                D2D1_RECT_F fitted = dest;
                const float sourceAspect = static_cast<float>(item.w) /
                                           static_cast<float>(std::max(1u, item.h));
                const float destAspect = destW / destH;
                if (sourceAspect > destAspect) {
                    const float height = destW / sourceAspect;
                    const float center = (dest.top + dest.bottom) * 0.5f;
                    fitted.top = center - height * 0.5f;
                    fitted.bottom = center + height * 0.5f;
                } else if (sourceAspect < destAspect) {
                    const float width = destH * sourceAspect;
                    const float center = (dest.left + dest.right) * 0.5f;
                    fitted.left = center - width * 0.5f;
                    fitted.right = center + width * 0.5f;
                }
                dc->DrawBitmap(bitmap.get(), &fitted, std::clamp(opacity, 0.0f, 1.0f),
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                Trim();
                return PreviewDrawResult::Bitmap;
            }
            if (item.failed) return PreviewDrawResult::Failed;
        }
        queue_request();
    }
    return PreviewDrawResult::Pending;
}

bool ThumbnailCache::Properties(const std::wstring& path, DWORD attrs, uint64_t generation,
                                uint64_t modified, uint64_t size,
                                std::vector<PreviewProperty>& properties, Consumer consumer) {
    if (path.empty() || fs::IsCloudPlaceholder(attrs)) return false;
    const std::wstring identity = Key(path, 0, modified, size);
    const std::wstring key = identity + L":properties";
    std::lock_guard lock(mutex_);
    KeepVisible(consumer, key);
    SelectDetails(consumer, identity, generation);
    if (auto it = items_.find(key); it != items_.end()) {
        Touch(it->second);
        properties = it->second.properties;
        return !it->second.failed;
    }
    Request request;
    request.generation = generation; request.attrs = attrs;
    request.kind = ipc::PreviewRequestKind::Properties;
    request.details = true; request.path = path; request.key = key; request.identity = identity;
    QueueRequest(std::move(request), consumer);
    return false;
}
bool ThumbnailCache::CachedProperties(const std::wstring& path, uint64_t modified,
                                      uint64_t size,
                                      std::vector<PreviewProperty>& properties) {
    if (path.empty()) return false;
    const std::wstring key = Key(path, 0, modified, size) + L":properties";
    std::lock_guard lock(mutex_);
    const auto it = items_.find(key);
    if (it == items_.end() || it->second.failed) return false;
    Touch(it->second);
    properties = it->second.properties;
    return true;
}
void ThumbnailCache::Touch(Item& item) {
    lru_.splice(lru_.end(), lru_, item.lru_position);
}

bool ThumbnailCache::StoreResult(const Request& req, Item result) {
    std::lock_guard lock(mutex_);
    // An old decode must not remove a replacement request queued after Evict().
    if (req.epoch != epoch_.load(std::memory_order_relaxed)) return false;
    auto pending = pending_.find(req.key);
    if (pending == pending_.end() || pending->second.id != req.id) return false;
    std::array<bool, 2> notify{};
    for (size_t index = 0; index < consumers_.size(); ++index) {
        const auto& interest = pending->second.interests[index];
        const auto& state = consumers_[index];
        notify[index] = interest.active && interest.epoch == state.epoch &&
            (!interest.details || (pending->second.identity == state.details_identity &&
                                  interest.generation == state.details_generation));
    }
    const Pending retry_interests = pending->second;
    pending_.erase(pending);
    if (!notify[0] && !notify[1]) return false;

    result.failure_attempts = result.failed ? req.failure_attempts + 1 : 0;
    const bool retry = result.failed && result.retryable &&
        result.failure_attempts < kMaxFailureAttempts;
    const uint32_t attempts = result.failure_attempts;

    if (auto old = items_.find(req.key); old != items_.end()) {
        cache_bytes_ -= old->second.cost;
        lru_.erase(old->second.lru_position);
        items_.erase(old);
    }
    if (result.frame_count > 1) {
        size_t frames = 0;
        for (const auto& [key, item] : items_) {
            if (item.frame_count > 1 && item.animation_identity == req.identity) ++frames;
        }
        if (frames >= 4) {
            for (auto lru = lru_.begin(); lru != lru_.end(); ++lru) {
                auto cached = items_.find(*lru);
                if (cached->second.frame_count > 1 &&
                    cached->second.animation_identity == req.identity) {
                    cache_bytes_ -= cached->second.cost;
                    items_.erase(cached);
                    lru_.erase(lru);
                    break;
                }
            }
        }
    }
    cache_bytes_ += result.cost;
    lru_.push_back(req.key);
    result.lru_position = std::prev(lru_.end());
    items_.emplace(req.key, std::move(result));
    if (retry) {
        Request next = req;
        next.id = next_id_++;
        next.failure_attempts = attempts;
        next.retry_due = GetTickCount64() + kFailureRetryDelayMs;
        Pending interests = retry_interests;
        interests.id = next.id;
        pending_[req.key] = std::move(interests);
        queue_.push_back(std::move(next));
        cv_.notify_one();
    }
    Trim();
    // Serialize notifications with consumer detachment so a destroyed HWND is never reused.
    for (size_t index = 0; index < consumers_.size(); ++index)
        if (notify[index] && consumers_[index].hwnd)
            InvalidateRect(consumers_[index].hwnd, nullptr, FALSE);
    return true;
}
void ThumbnailCache::Trim() {
    constexpr size_t kCacheBudget = 16ull * 1024ull * 1024ull;
    while (!lru_.empty() && (lru_.size() > 128 || cache_bytes_ > kCacheBudget)) {
        const auto victim = std::find_if(lru_.begin(), lru_.end(), [&](const std::wstring& key) {
            return std::none_of(consumers_.begin(), consumers_.end(), [&](const ConsumerState& state) {
                return state.visible_keys.contains(key) || state.frame_keys.contains(key);
            });
        });
        // The visible working set may exceed the idle budget. Evicting it
        // would alternate every redraw between covers and Shell icons.
        if (victim == lru_.end()) break;
        auto oldest = items_.find(*victim);
        cache_bytes_ -= oldest->second.cost;
        items_.erase(oldest);
        lru_.erase(victim);
    }
}

bool ThumbnailCache::FetchResponse(const Request& req, Item& result, size_t lane) {
    const auto lane_pipe = lane ? hosts_[lane - 1].pipe : this->pipe_;
    if (!stop_event_ || !running_) return false;
    ipc::PipeIoOptions options;
    options.cancel_event = stop_event_;
    options.deadline = GetTickCount64() + 1000;
    const auto read = [&](void* data, DWORD bytes) { return ipc::PipeRead(lane_pipe, data, bytes, options); };
    const auto write = [&](const void* data, DWORD bytes) { return ipc::PipeWrite(lane_pipe, data, bytes, options); };
    try {
        if (req.path.empty() || req.path.size() > 32768) return false;
        ipc::PreviewRequest wire;
        wire.request_id = req.id; wire.generation = req.generation; wire.kind = req.kind;
        wire.pixel_size = req.pixels; wire.attrs = req.attrs; wire.frame_index = req.frame_index;
        wire.path_chars = static_cast<uint32_t>(req.path.size());
        ipc::PreviewResponse response{};
        if (!write(&wire, sizeof(wire)) || !write(req.path.data(), wire.path_chars * sizeof(wchar_t)) ||
            !read(&response, sizeof(response))) return false;
        ipc::PreviewResponseLayout layout;
        if (!ipc::ValidatePreviewResponse(wire, response, layout)) return false;
        if (before_response_body_) before_response_body_();
        const auto read_string = [&](std::wstring& text, uint32_t chars) {
            text.resize(chars);
            return read(text.data(), chars * sizeof(wchar_t));
        };
        std::wstring mapping;
        if (!read_string(mapping, response.mapping_chars) ||
            (response.mapping_chars && (!mapping.starts_with(L"Local\\PulsePreviewMap-") ||
                mapping.find(L'\0') != std::wstring::npos)) ||
            !read_string(result.text, response.text_chars) || !read_string(result.error, response.error_chars)) return false;
        result.properties.reserve(response.property_count);
        for (uint32_t i = 0; i < response.property_count; ++i) {
            PreviewProperty property;
            uint32_t label_chars = 0, value_chars = 0;
            if (!read(&label_chars, sizeof(label_chars)) || label_chars > ipc::kPreviewMaxPropertyLabelChars ||
                !read_string(property.label, label_chars) || !read(&value_chars, sizeof(value_chars)) ||
                value_chars > ipc::kPreviewMaxPropertyValueChars || !read_string(property.value, value_chars)) return false;
            result.properties.push_back(std::move(property));
        }
        if (response.status == 0 && response.mapping_chars) {
            struct Mapping {
                HANDLE handle = nullptr;
                void* view = nullptr;
                ~Mapping() { if (view) UnmapViewOfFile(view); if (handle) CloseHandle(handle); }
            } mapped;
            mapped.handle = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping.c_str());
            if (!mapped.handle) return false;
            mapped.view = MapViewOfFile(mapped.handle, FILE_MAP_READ, 0, 0, layout.pixel_bytes);
            if (!mapped.view) return false;
            result.pixels.resize(layout.pixel_bytes);
            std::memcpy(result.pixels.data(), mapped.view, layout.pixel_bytes);
            result.w = response.width; result.h = response.height; result.stride = response.stride;
        }
        if (response.mapping_chars) { const uint8_t ack = 1; if (!write(&ack, sizeof(ack))) return false; }
        if (WaitForSingleObject(stop_event_, 0) != WAIT_TIMEOUT || GetTickCount64() >= options.deadline) return false;
        result.kind = response.kind;
        result.truncated = (response.flags & ipc::kPreviewFlagTruncated) != 0;
        result.bytes_read = response.bytes_read;
        result.frame_count = response.frame_count; result.frame_delay_ms = response.frame_delay_ms;
        result.loop_count = response.loop_count; result.frame_index = req.frame_index;
        result.source_width = response.source_width; result.source_height = response.source_height;
        result.cost = result.pixels.size() + (result.text.size() + result.error.size()) * sizeof(wchar_t);
        for (const auto& property : result.properties)
            result.cost += (property.label.size() + property.value.size()) * sizeof(wchar_t);
        result.failed = response.status != 0;
        result.retryable = result.failed && (response.status == 2 || result.error == L"provider-failed" ||
            result.error == L"path-unavailable" || result.error == L"image-decode-failed" ||
            result.error == L"content-read-failed" || result.error == L"svg-render-failed");
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

std::deque<ThumbnailCache::Request>::iterator ThumbnailCache::NextRequest(size_t lane, ULONGLONG now) {
    auto ready = queue_.end();
    int best = -1;
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->retry_due > now || (lane == 0 && it->prefetch)) continue;
        int priority = it->prefetch ? 0 : 1;
        for (const auto& state : consumers_)
            if (state.visible_keys.contains(it->key) || state.frame_keys.contains(it->key)) priority = 2;
        if (it->details) priority = 3;
        if (priority > best) { best = priority; ready = it; }
    }
    return ready;
}

void ThumbnailCache::Worker(size_t lane) {
    Connect(lane); // warm only on this lane's background thread
    while (running_) {
        Request req;
        {
            std::unique_lock lock(mutex_);
            while (running_) {
                const ULONGLONG now = GetTickCount64();
                auto ready = NextRequest(lane, now);
                if (ready != queue_.end()) {
                    req = std::move(*ready);
                    queue_.erase(ready);
                    break;
                }
                if (queue_.empty() || (lane == 0 && std::all_of(queue_.begin(), queue_.end(),
                    [](const Request& r) { return r.prefetch; }))) cv_.wait(lock);
                else {
                    ULONGLONG next_due = MAXULONGLONG;
                    for (const auto& queued : queue_)
                        if (lane != 0 || !queued.prefetch) next_due = std::min(next_due, queued.retry_due);
                    cv_.wait_for(lock, std::chrono::milliseconds(next_due - now));
                }
            }
            if (!running_) break;
        }
        Item result;
        const bool grid = req.kind == ipc::PreviewRequestKind::Thumbnail;
        ThumbnailStoreKey disk_key{req.path, req.attrs, req.pixels, req.modified, req.size};
        ThumbnailFileIdentity before{};
        const bool verified = grid && ReadThumbnailFileIdentity(disk_key, before);
        StoredThumbnail stored;
        bool disk_hit = verified && ThumbnailDiskCache::Shared().Load(disk_key, stored);
        const auto started = GetTickCount64();
        bool ok = disk_hit;
        if (disk_hit) {
            result.pixels = std::move(stored.pixels);
            result.w = stored.width; result.h = stored.height; result.stride = stored.width * 4;
            result.source_width = stored.source_width; result.source_height = stored.source_height;
            result.kind = ipc::PreviewContentKind::Bitmap; result.cost = result.pixels.size();
        } else ok = Connect(lane) && FetchResponse(req, result, lane);
        if (!running_) break;
        {
            std::lock_guard lock(mutex_);
            const auto pending = pending_.find(req.key);
            if (req.epoch != epoch_.load(std::memory_order_relaxed) || pending == pending_.end() ||
                pending->second.id != req.id) continue;
        }
        if (verified) {
            ThumbnailFileIdentity after{};
            if (!ReadThumbnailFileIdentity(disk_key, after) || !(before == after)) ok = false;
        }
        if (verified && ok && !disk_hit && !result.failed && result.kind == ipc::PreviewContentKind::Bitmap) {
            stored = {result.pixels, result.w, result.h, result.source_width, result.source_height, 0};
            ThumbnailDiskCache::Shared().Save(disk_key,
                static_cast<uint32_t>(std::min<ULONGLONG>(GetTickCount64() - started, UINT32_MAX)), stored, &before);
        }
        if (!ok) {
            result = {};
            result.failed = true;
            result.retryable = true;
        }
        result.animation_identity = req.identity;
        StoreResult(req, std::move(result));
        if (!ok) StopChild(lane);
    }
}
} // namespace pulse::ui
