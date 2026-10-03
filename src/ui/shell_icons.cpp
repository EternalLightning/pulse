// shell_icons.cpp
#include "shell_icons.h"
#include "../common/path_utils.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wincodec.h>
#include <cwctype>
#include <algorithm>
#include <cmath>
#include <array>

namespace pulse::ui {

namespace {

// Shell handlers can expose an icon per path (not just per extension). Keep
// those maps bounded because a long scroll through executable/link files
// otherwise retains every path seen during the session.
constexpr size_t kExactIndexLimit = 4096;
constexpr size_t kBitmapCacheLimit = 128;
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
    hwnd_ = nullptr;
    Reset();
}

void ShellIconCache::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (dc_ == dc) return;
    dc_ = dc;
    bitmaps_.clear();
}

void ShellIconCache::SetScale(float scale) {
    if (std::abs(scale_ - scale) <= 0.001f) return;
    scale_ = scale;
}

void ShellIconCache::SetNotifyWindow(HWND hwnd) {
    hwnd_ = hwnd;
    if (worker_) {
        std::lock_guard lock(worker_->mutex);
        worker_->notify = hwnd;
    }
    if (hwnd) {
        GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        GenericIndex(L"", false, FILE_ATTRIBUTE_NORMAL);
    }
}

ShellIconCache::WorkerState::~WorkerState() {
    if (thread) CloseHandle(thread);
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
        state->notify = hwnd_;
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
            worker->notify = nullptr;
        }
        worker->cv.notify_all();
        auto& pool = Workers();
        std::lock_guard lock(pool.mutex);
        pool.slots[worker->slot].retired = true;
        pool.ReapLocked();
    }
    bitmaps_.clear();
    for (auto& [_, list] : image_lists_)
        if (list) list->Release();
    image_lists_.clear();
    wic_.reset();
    dc_ = nullptr;
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

IImageList* ShellIconCache::EnsureImageList(int id) {
    if (const auto cached = image_lists_.find(id); cached != image_lists_.end())
        return cached->second;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(id, IID_IImageList, reinterpret_cast<void**>(&list))) || !list)
        return nullptr;
    image_lists_.emplace(id, list);
    return list;
}

bool ShellIconCache::EnsureWic() {
    if (wic_.get()) return true;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&wic_))) && wic_.get();
}

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
            state->queued.erase(state->queue.front());
            state->queue.pop();
        }
        if (const auto retry = state->retry_after.find(path);
            retry != state->retry_after.end() && GetTickCount64() < retry->second) return;
        state->queued.insert(path);
        state->queue.push(path);
    }
    state->cv.notify_one();
}

DWORD WINAPI ShellIconCache::WorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<WorkerState>> argument(
        static_cast<std::shared_ptr<WorkerState>*>(parameter));
    const auto state = *argument;
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring prefix = std::wstring(1, L'\x1f') + L"GEN:";
    for (;;) {
        std::wstring path;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] { return state->stop || !state->queue.empty(); });
            if (state->stop) break;
            path = std::move(state->queue.front());
            state->queue.pop();
        }
        SHFILEINFOW info{};
        int index = -1;
        const bool generic = path.starts_with(prefix);
        const std::wstring key = generic ? path.substr(prefix.size()) : std::wstring{};
#ifdef PULSE_SHELL_ICONS_TESTING
        if (state->query_hook) {
            index = state->query_hook(path);
        } else
#endif
        if (generic) {
            const bool dir = key == L"<dir>";
            const std::wstring query = dir ? L"dummy" : (key == L"<file>" ? L"dummy" : L"dummy" + key);
            const DWORD attrs = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (SHGetFileInfoW(query.c_str(), attrs, &info, sizeof(info),
                               SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) index = info.iIcon;
        } else {
            const std::wstring query = ShellPath(path);
            if (SHGetFileInfoW(query.c_str(), 0, &info, sizeof(info),
                               SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) index = info.iIcon;
            if (info.hIcon) DestroyIcon(info.hIcon);
        }
        {
            std::lock_guard lock(state->mutex);
            // A late Shell result cannot populate a replacement cache or notify
            // a destroyed/reused window. Reset takes this same short gate.
            if (state->stop) break;
            state->queued.erase(path);
            if (index >= 0) {
                state->retry_after.erase(path);
                if (generic) {
                    if (state->generic_index.size() >= 512) {
                        const auto victim = std::find_if(state->generic_index.begin(), state->generic_index.end(),
                            [](const auto& item) { return item.first != L"<dir>" && item.first != L"<file>"; });
                        if (victim != state->generic_index.end()) state->generic_index.erase(victim);
                    }
                    state->generic_index[key] = index;
                } else {
                    if (state->exact_index.size() >= kExactIndexLimit) {
                        const auto oldest = std::min_element(state->last_used.begin(), state->last_used.end(),
                            [](const auto& a, const auto& b) { return a.second < b.second; });
                        if (oldest != state->last_used.end()) {
                            state->exact_index.erase(oldest->first);
                            state->last_used.erase(oldest);
                        }
                    }
                    state->exact_index[path] = index;
                    state->last_used[path] = ++state->access_clock;
                }
            } else {
                if (state->retry_after.size() >= kExactIndexLimit) state->retry_after.erase(state->retry_after.begin());
                state->retry_after[path] = GetTickCount64() + 2000;
            }
            if (state->notify) InvalidateRect(state->notify, nullptr, FALSE);
        }
    }
    if (SUCCEEDED(com_hr)) CoUninitialize();
    return 0;
}

ComPtr<ID2D1Bitmap> ShellIconCache::BitmapFromIcon(HICON icon) {
    ComPtr<ID2D1Bitmap> bitmap;
    if (!icon || !dc_ || !EnsureWic()) return bitmap;
    ComPtr<IWICBitmap> wicBitmap;
    if (FAILED(wic_->CreateBitmapFromHICON(icon, &wicBitmap)) || !wicBitmap.get()) {
        return bitmap;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic_->CreateFormatConverter(&converter)) || !converter.get()) {
        return bitmap;
    }
    if (FAILED(converter->Initialize(wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) {
        return bitmap;
    }
    dc_->CreateBitmapFromWicBitmap(converter.get(), nullptr, &bitmap);
    return bitmap;
}

ID2D1Bitmap* ShellIconCache::BitmapForIndex(int index, int list_id) {
    if (index < 0 || !dc_) return nullptr;
    IImageList* image_list = EnsureImageList(list_id);
    if (!image_list) return nullptr;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(list_id)) << 32) |
        static_cast<uint32_t>(index);
    auto it = bitmaps_.find(key);
    if (it != bitmaps_.end()) return it->second.get();
    HICON icon = nullptr;
    if (FAILED(image_list->GetIcon(index, ILD_TRANSPARENT, &icon)) || !icon) {
        return nullptr;
    }
    ComPtr<ID2D1Bitmap> bitmap = BitmapFromIcon(icon);
    DestroyIcon(icon);
    if (!bitmap.get()) return nullptr;
    ID2D1Bitmap* raw = bitmap.get();
    if (bitmaps_.size() >= kBitmapCacheLimit) bitmaps_.erase(bitmaps_.begin());
    bitmaps_[key] = std::move(bitmap);
    return raw;
}

bool ShellIconCache::DrawSystemIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest, int index) {
    if (!dc || index < 0) return false;
    const float desired = std::max(dest.right - dest.left, dest.bottom - dest.top);
    auto* bitmap = BitmapForIndex(index, ImageListId(desired));
    if (!bitmap) return false;
    dc->DrawBitmap(bitmap, &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
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
    int index = -1;
    const bool exact = NeedsExactIcon(name, is_dir, path) && !path.empty();
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
