// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// shell_icons.cpp
#include "shell_icons.h"
#include "../fs/cloud_state.h"
#include "../common/path_utils.h"
#include "../ipc/shell_query_client.h"

#include <shellapi.h>
#ifdef PULSE_SHELL_ICONS_TESTING
#include <wincodec.h>
#include "../shell_host/shell_query_icons.h"
#endif
#include <cwctype>
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>

namespace pulse::ui {

namespace {

// Shell handlers can expose an icon per path (not just per extension). Keep
// those maps bounded because a long scroll through executable/link files
// otherwise retains every path seen during the session.
constexpr size_t kExactIndexLimit = 4096;
constexpr size_t kBitmapCacheLimit = 128;
constexpr size_t kPixelCacheLimit = 32 * 1024 * 1024;
constexpr uint32_t kWorkerLimit = 4;
struct WorkerSlot {
    std::shared_ptr<void> owner;
    HANDLE thread = nullptr;
    bool retired = false;
};
struct WorkerPool {
    std::mutex mutex;
    std::array<WorkerSlot, kWorkerLimit> slots;
    void ReapLocked() {
        for (auto& slot : slots) {
            if (slot.retired && slot.thread && WaitForSingleObject(slot.thread, 0) == WAIT_OBJECT_0)
                slot = {};
        }
    }
};
WorkerPool& Workers() {
    // Lifetime includes permanently blocked Shell calls; never destroy their
    // state while the process is tearing down static objects.
    static auto* pool = new WorkerPool;
    return *pool;
}
#ifdef PULSE_SHELL_ICONS_TESTING
std::mutex g_hook_mutex;
std::function<int(const std::wstring&)> g_query_hook;
#endif

std::wstring LowerExt(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

} // namespace

ShellIconCache::ShellIconCache() = default;

ShellIconCache::~ShellIconCache() {
    notify_->Set(nullptr);
    Reset();
}

void ShellIconCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (device_binding_.Bind(dc)) bitmaps_.clear();
    dc_ = device_binding_.Dc();
}

void ShellIconCache::SetScale(float scale) {
    if (std::abs(scale_ - scale) <= 0.001f) return;
    scale_ = scale;
}

void ShellIconCache::SetNotifyWindow(HWND hwnd) {
    notify_->Set(hwnd);
    if (hwnd) {
        GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        GenericIndex(L"", false, FILE_ATTRIBUTE_NORMAL);
    }
}

void ShellIconCache::AddNotifyWindow(HWND hwnd) {
    notify_->Add(hwnd);
    if (hwnd) {
        GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        GenericIndex(L"", false, FILE_ATTRIBUTE_NORMAL);
    }
}

void ShellIconCache::RemoveNotifyWindow(HWND hwnd) {
    notify_->Remove(hwnd);
}

ShellIconCache::WorkerState::~WorkerState() {
    if (thread) CloseHandle(thread);
}

void ShellIconCache::WorkerState::RemovePixels(int index) {
    const auto found = pixels.find(index);
    if (found == pixels.end()) return;
    for (const auto& icon : *found->second) pixel_bytes -= icon.pixels.size();
    pixels.erase(found);
    eviction_generation.fetch_add(1, std::memory_order_release);
}

ShellIconCache::ResourceUsage ShellIconCache::Resources() {
    auto& pool = Workers();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    ResourceUsage usage{};
    usage.limit = kWorkerLimit;
    for (const auto& slot : pool.slots) {
        if (slot.thread) ++usage.live;
        if (slot.retired) ++usage.retired;
    }
    return usage;
}

bool ShellIconCache::EnsureWorker() {
    if (worker_) return true;
    auto& pool = Workers();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    for (uint32_t i = 0; i < kWorkerLimit; ++i) {
        if (pool.slots[i].thread) continue;
        auto state = std::make_shared<WorkerState>();
        state->slot = i;
        state->notify = notify_;
#ifdef PULSE_SHELL_ICONS_TESTING
        {
            std::lock_guard hook_lock(g_hook_mutex);
            state->query_hook = g_query_hook;
        }
#endif
        auto* argument = new std::shared_ptr<WorkerState>(state);
        state->thread = CreateThread(nullptr, 0, WorkerMain, argument, 0, nullptr);
        if (!state->thread) {
            delete argument;
            return false;
        }
        pool.slots[i].owner = state;
        pool.slots[i].thread = state->thread;
        worker_ = std::move(state);
        return true;
    }
    return false;
}

void ShellIconCache::Reset() {
    if (const auto worker = std::move(worker_)) {
        {
            std::lock_guard lock(worker->mutex);
            worker->stop = true;
            worker->notify.reset();
        }
        worker->cv.notify_all();
        auto& pool = Workers();
        std::lock_guard lock(pool.mutex);
        pool.slots[worker->slot].retired = true;
        pool.ReapLocked();
    }
    bitmaps_.clear();
    bitmap_eviction_generation_ = 0;
#ifdef PULSE_SHELL_ICONS_TESTING
    wic_.reset();
#endif
    dc_ = nullptr;
    device_binding_.Bind(nullptr);
}

void ShellIconCache::Invalidate(const std::wstring& path) {
    if (path.empty() || !worker_) return;
    std::wstring root = ShellPath(path);
    if (root.empty()) return;
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    const auto matches = [&](const std::wstring& candidate) {
        const auto key = ShellPath(candidate);
        if (key.size() < root.size() || CompareStringOrdinal(key.data(), static_cast<int>(root.size()),
            root.data(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) return false;
        return key.size() == root.size() || root.back() == L'\\' || root.back() == L'/' ||
            key[root.size()] == L'\\' || key[root.size()] == L'/';
    };
    std::lock_guard lock(worker_->mutex);
    std::erase_if(worker_->exact_index, [&](const auto& item) {
        if (!matches(item.first)) return false;
        worker_->RemovePixels(item.second);
        worker_->last_used.erase(item.first);
        return true;
    });
    std::erase_if(worker_->retry_after, [&](const auto& item) { return matches(item.first); });
    // Removing the ticket also rejects an in-flight result. A later draw can
    // enqueue the changed file immediately, without waiting for the old query.
    std::erase_if(worker_->queued, [&](const auto& item) { return matches(item.first); });
}

#ifdef PULSE_SHELL_ICONS_TESTING
void ShellIconCache::SetQueryHookForTest(std::function<int(const std::wstring&)> hook) {
    std::lock_guard lock(g_hook_mutex);
    g_query_hook = std::move(hook);
}
#endif

int ShellIconCache::ImageListId(float desired_pixels) noexcept {
    if (desired_pixels <= 16.0f) return SHIL_SMALL;
    if (desired_pixels <= 32.0f) return SHIL_LARGE;
    if (desired_pixels <= 64.0f) return SHIL_EXTRALARGE;
    return SHIL_JUMBO;
}

#ifdef PULSE_SHELL_ICONS_TESTING
bool ShellIconCache::EnsureWic() {
    if (wic_.get()) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic_))) && wic_.get();
}
#endif

bool ShellIconCache::NeedsExactIcon(const std::wstring& name, bool is_dir,
                                    const std::wstring& path) {
    (void)name;
    (void)is_dir;
    // Folder customizations and registered icon handlers are path-dependent.
    return !path.empty();
}

int ShellIconCache::GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs) {
    std::wstring key = is_dir ? L"<dir>" : LowerExt(name);
    if (key.empty()) key = L"<file>";
    if (!EnsureWorker()) return -1;
    {
        std::lock_guard lock(worker_->mutex);
        if (const auto cached = worker_->generic_index.find(key);
            cached != worker_->generic_index.end()) return cached->second;
    }
    (void)attrs;
    RequestExact(std::wstring(1, L'\x1f') + L"GEN:" + key);
    return -1;
}

void ShellIconCache::RequestExact(const std::wstring& path) {
    if (path.empty() || !EnsureWorker()) return;
    const auto state = worker_;
    {
        std::lock_guard lock(state->mutex);
        if (state->exact_index.contains(path)) {
            state->last_used[path] = ++state->access_clock;
            return;
        }
        if (state->queued.contains(path)) return;
        if (state->queue.size() >= kExactIndexLimit) {
            const auto& old = state->queue.front();
            if (const auto ticket = state->queued.find(old.path);
                ticket != state->queued.end() && ticket->second == old.id) state->queued.erase(ticket);
            state->queue.pop();
        }
        if (const auto retry = state->retry_after.find(path);
            retry != state->retry_after.end() && GetTickCount64() < retry->second) return;
        const uint64_t id = ++state->next_request_id;
        state->queued.emplace(path, id);
        state->queue.push({path, id});
    }
    state->cv.notify_one();
}

DWORD WINAPI ShellIconCache::WorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<WorkerState>> argument(
        static_cast<std::shared_ptr<WorkerState>*>(parameter));
    const auto state = *argument;
    const std::wstring prefix = std::wstring(1, L'\x1f') + L"GEN:";
    for (;;) {
        std::wstring path;
        uint64_t request_id = 0;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] { return state->stop || !state->queue.empty(); });
            if (state->stop) break;
            path = std::move(state->queue.front().path);
            request_id = state->queue.front().id;
            state->queue.pop();
            const auto ticket = state->queued.find(path);
            if (ticket == state->queued.end() || ticket->second != request_id) continue;
        }
        ipc::ShellQueryRequest request;
        request.kind = ipc::ShellQueryKind::Icon;
        ipc::ShellQueryResult result;
        const bool generic = path.starts_with(prefix);
        const std::wstring key = generic ? path.substr(prefix.size()) : std::wstring{};
#ifdef PULSE_SHELL_ICONS_TESTING
        if (state->query_hook) {
            if (state->query_hook(path) >= 0) {
                result.error = S_OK;
                result.icons.push_back({1, 1, 1, 4, {0x33, 0x66, 0x99, 0xff}});
            }
        } else
#endif
        {
            if (generic) {
                const bool dir = key == L"<dir>";
                request.path = key == L"<dir>" || key == L"<file>" ? L"dummy" : L"dummy" + key;
                request.attributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                request.use_attributes = true;
            } else {
                request.path = ShellPath(path);
            }
            ipc::QueryShell(request, result, [state] {
                std::lock_guard lock(state->mutex);
                return state->stop;
            });
        }
        {
            std::lock_guard lock(state->mutex);
            // A late Shell result cannot populate a replacement cache or notify
            // a destroyed/reused window. Reset takes this same short gate.
            if (state->stop) break;
            const auto ticket = state->queued.find(path);
            if (ticket == state->queued.end() || ticket->second != request_id) continue;
            state->queued.erase(ticket);
            if (SUCCEEDED(result.error) && !result.icons.empty()) {
                const auto evict_exact = [&] {
                    const auto oldest = std::min_element(state->last_used.begin(), state->last_used.end(),
                        [](const auto& a, const auto& b) { return a.second < b.second; });
                    if (oldest == state->last_used.end()) return false;
                    const auto found = state->exact_index.find(oldest->first);
                    if (found != state->exact_index.end()) {
                        state->RemovePixels(found->second);
                        state->exact_index.erase(found);
                    }
                    state->last_used.erase(oldest);
                    return true;
                };
                size_t bytes = 0;
                for (const auto& icon : result.icons) bytes += icon.pixels.size();
                while (state->pixel_bytes + bytes > kPixelCacheLimit) {
                    if (evict_exact()) continue;
                    const auto victim = std::find_if(state->generic_index.begin(), state->generic_index.end(),
                        [](const auto& item) { return item.first != L"<dir>" && item.first != L"<file>"; });
                    if (victim == state->generic_index.end()) break;
                    state->RemovePixels(victim->second);
                    state->generic_index.erase(victim);
                }
                const int index = state->next_index++;
                state->pixels[index] = std::make_shared<const std::vector<ipc::ShellIconPixels>>(std::move(result.icons));
                state->pixel_bytes += bytes;
                state->retry_after.erase(path);
                if (generic) {
                    if (state->generic_index.size() >= 512) {
                        const auto victim = std::find_if(state->generic_index.begin(), state->generic_index.end(),
                            [](const auto& item) { return item.first != L"<dir>" && item.first != L"<file>"; });
                        if (victim != state->generic_index.end()) {
                            state->RemovePixels(victim->second);
                            state->generic_index.erase(victim);
                        }
                    }
                    state->generic_index[key] = index;
                } else {
                    if (state->exact_index.size() >= kExactIndexLimit) {
                        evict_exact();
                    }
                    state->exact_index[path] = index;
                    state->last_used[path] = ++state->access_clock;
                }
            } else {
                if (state->retry_after.size() >= kExactIndexLimit) state->retry_after.erase(state->retry_after.begin());
                state->retry_after[path] = GetTickCount64() + 2000;
            }
            if (state->notify) state->notify->InvalidateAll();
        }
    }
    return 0;
}

#ifdef PULSE_SHELL_ICONS_TESTING
ComPtr<ID2D1Bitmap> ShellIconCache::BitmapFromIcon(HICON icon) {
    ComPtr<ID2D1Bitmap> bitmap;
    ipc::ShellIconPixels pixels;
    if (!dc_ || !EnsureWic() || !shell::ConvertShellIconPixels(icon, 4, pixels)) return bitmap;
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(pixels.width, pixels.height), pixels.pixels.data(),
                       pixels.stride, &properties, &bitmap);
    return bitmap;
}
#endif

bool ShellIconCache::PruneStaleBitmaps() {
    if (!worker_ || worker_->eviction_generation.load(std::memory_order_acquire) ==
            bitmap_eviction_generation_) return false;
    std::lock_guard lock(worker_->mutex);
    // Index IDs are never reused: a removed CPU index cannot produce another hit.
    std::erase_if(bitmaps_, [&](const auto& entry) {
        return !worker_->pixels.contains(static_cast<int>(static_cast<uint32_t>(entry.first)));
    });
    bitmap_eviction_generation_ = worker_->eviction_generation.load(std::memory_order_relaxed);
    return true;
}

ID2D1Bitmap* ShellIconCache::BitmapForIndex(int index, int list_id) {
    if (index < 0 || !dc_) return nullptr;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(list_id)) << 32) |
        static_cast<uint32_t>(index);
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.get();
    if (!worker_) return nullptr;
    std::shared_ptr<const std::vector<ipc::ShellIconPixels>> pixels;
    {
        std::lock_guard lock(worker_->mutex);
        const auto found = worker_->pixels.find(index);
        if (found == worker_->pixels.end()) return nullptr;
        pixels = found->second;
    }
    const auto icon = std::find_if(pixels->begin(), pixels->end(), [list_id](const auto& item) {
        return item.list_id == static_cast<uint32_t>(list_id);
    });
    if (icon == pixels->end()) return nullptr;
    ComPtr<ID2D1Bitmap> bitmap;
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(icon->width, icon->height), icon->pixels.data(),
                       icon->stride, &properties, &bitmap);
    if (!bitmap.get()) return nullptr;
    ID2D1Bitmap* raw = bitmap.get();
    if (bitmaps_.size() >= kBitmapCacheLimit) bitmaps_.erase(bitmaps_.begin());
    bitmaps_[key] = std::move(bitmap);
    return raw;
}

bool ShellIconCache::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                          const std::wstring& path, const std::wstring& name,
                          bool is_dir, DWORD attrs) {
    if (!dc) return false;
    const float desired = std::max(dest.right - dest.left, dest.bottom - dest.top);
    ID2D1Bitmap* bitmap = BitmapFor(path, name, is_dir, attrs, desired);
    if (!bitmap) return false;
    dc->DrawBitmap(bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   nullptr, nullptr);
    return true;
}


ID2D1Bitmap* ShellIconCache::BitmapFor(const std::wstring& path, const std::wstring& name,
                                       bool is_dir, DWORD attrs, float desired_dips) {
    PruneStaleBitmaps();
    int index = -1;
    const bool exact = !fs::IsCloudPlaceholder(attrs) && NeedsExactIcon(name, is_dir, path);
    if (exact && worker_) {
        std::lock_guard lock(worker_->mutex);
        const auto it = worker_->exact_index.find(path);
        if (it != worker_->exact_index.end()) {
            index = it->second;
            worker_->last_used[path] = ++worker_->access_clock;
        }
    }
    if (index < 0 && exact) RequestExact(path);
    if (index < 0) index = GenericIndex(name, is_dir, attrs);
    const int list_id = ImageListId(desired_dips);
    if (auto* bitmap = BitmapForIndex(index, list_id)) return bitmap;
    return BitmapForIndex(GenericIndex(L"", is_dir, attrs), list_id);
}
} // namespace pulse::ui
