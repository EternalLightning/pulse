// shell_icons.h — isolated Windows Shell icon pixels for file rows.
#pragma once
#include "ui_compositor.h"
#include "d2d_device_binding.h"
#include "repaint_targets.h"
#include "../ipc/shell_query_protocol.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <unordered_set>
#include <memory>
#include <atomic>
#ifdef PULSE_SHELL_ICONS_TESTING
#include <functional>
#endif

#ifdef PULSE_SHELL_ICONS_TESTING
struct IWICImagingFactory;
#endif

namespace pulse::ui {

class ShellIconCache {
public:
    ShellIconCache();
    ~ShellIconCache();
    ShellIconCache(const ShellIconCache&) = delete;
    ShellIconCache& operator=(const ShellIconCache&) = delete;

    void SetDeviceContext(ID2D1DeviceContext* dc);
    void SetScale(float scale);
    void SetNotifyWindow(HWND hwnd);
    void AddNotifyWindow(HWND hwnd);
    void RemoveNotifyWindow(HWND hwnd);
    // Cancels the isolated query without joining. Old CPU-only state remains owned
    // until actual thread exit; device resources are never used by that worker.
    void Reset();
    // Invalidate a changed path and its descendants without blocking on Shell.
    void Invalidate(const std::wstring& path);

    struct ResourceUsage {
        uint32_t live = 0;
        uint32_t retired = 0;
        uint32_t limit = 0;
    };
    static ResourceUsage Resources();
#ifdef PULSE_SHELL_ICONS_TESTING
    static void SetQueryHookForTest(std::function<int(const std::wstring&)> hook);
#endif

    // Returns false while native icons are loading or the device is unavailable.
    bool Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
              const std::wstring& path, const std::wstring& name,
              bool is_dir, DWORD attrs);

    // Icon bitmap at (about) desired_dips, or nullptr while unresolved.
    ID2D1Bitmap* BitmapFor(const std::wstring& path, const std::wstring& name,
                           bool is_dir, DWORD attrs, float desired_dips);

private:
    friend struct ShellIconCacheTestAccess;
    struct WorkerState {
        ~WorkerState();
        void RemovePixels(int index);
        std::mutex mutex;
        std::condition_variable cv;
        struct Request { std::wstring path; uint64_t id; };
        std::queue<Request> queue;
        std::unordered_map<std::wstring, uint64_t> queued;
        uint64_t next_request_id = 0;
        std::unordered_map<std::wstring, int> generic_index;
        std::unordered_map<std::wstring, int> exact_index;
        std::unordered_map<std::wstring, ULONGLONG> retry_after;
        std::unordered_map<std::wstring, uint64_t> last_used;
        std::unordered_map<int, std::shared_ptr<const std::vector<ipc::ShellIconPixels>>> pixels;
        size_t pixel_bytes = 0;
        std::atomic<uint64_t> eviction_generation{0};
        int next_index = 1;
        uint64_t access_clock = 0;
        std::shared_ptr<RepaintTargets> notify;
        bool stop = false;
        HANDLE thread = nullptr;
        uint32_t slot = 0;
#ifdef PULSE_SHELL_ICONS_TESTING
        std::function<int(const std::wstring&)> query_hook;
#endif
    };
    static int ImageListId(float desired_pixels) noexcept;
#ifdef PULSE_SHELL_ICONS_TESTING
    bool EnsureWic();
    ComPtr<ID2D1Bitmap> BitmapFromIcon(HICON icon);
#endif
    bool EnsureWorker();
    int GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs);
    void RequestExact(const std::wstring& path);
    ID2D1Bitmap* BitmapForIndex(int index, int list_id);
    bool PruneStaleBitmaps();
    static DWORD WINAPI WorkerMain(void* parameter);
    static bool NeedsExactIcon(const std::wstring& name, bool is_dir,
                               const std::wstring& path);

    ID2D1DeviceContext* dc_ = nullptr;
    D2DDeviceBinding device_binding_;
    std::shared_ptr<RepaintTargets> notify_ = std::make_shared<RepaintTargets>();
    float scale_ = 1.0f;
#ifdef PULSE_SHELL_ICONS_TESTING
    ComPtr<IWICImagingFactory> wic_;
#endif
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::shared_ptr<WorkerState> worker_;
    uint64_t bitmap_eviction_generation_ = 0;
};

} // namespace pulse::ui
