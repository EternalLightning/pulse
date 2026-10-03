// shell_icons.h — Windows Shell image-list icons for file rows.
#pragma once
#include "ui_compositor.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <unordered_set>
#include <memory>
#ifdef PULSE_SHELL_ICONS_TESTING
#include <functional>
#endif

struct IImageList;
struct IWICImagingFactory;

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
    // Never joins a possibly stuck Shell call. Old CPU-only state remains owned
    // until actual thread exit; device resources are never used by that worker.
    void Reset();

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
    bool DrawSystemIcon(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest, int index);

    // Icon bitmap at (about) desired_dips, or nullptr while unresolved.
    ID2D1Bitmap* BitmapFor(const std::wstring& path, const std::wstring& name,
                           bool is_dir, DWORD attrs, float desired_dips);

private:
    friend struct ShellIconCacheTestAccess;
    struct WorkerState {
        ~WorkerState();
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::wstring> queue;
        std::unordered_set<std::wstring> queued;
        std::unordered_map<std::wstring, int> generic_index;
        std::unordered_map<std::wstring, int> exact_index;
        std::unordered_map<std::wstring, ULONGLONG> retry_after;
        std::unordered_map<std::wstring, uint64_t> last_used;
        uint64_t access_clock = 0;
        HWND notify = nullptr;
        bool stop = false;
        HANDLE thread = nullptr;
        uint32_t slot = 0;
#ifdef PULSE_SHELL_ICONS_TESTING
        std::function<int(const std::wstring&)> query_hook;
#endif
    };
    static int ImageListId(float desired_pixels) noexcept;
    IImageList* EnsureImageList(int list_id);
    bool EnsureWic();
    bool EnsureWorker();
    int GenericIndex(const std::wstring& name, bool is_dir, DWORD attrs);
    void RequestExact(const std::wstring& path);
    ID2D1Bitmap* BitmapForIndex(int index, int list_id);
    ComPtr<ID2D1Bitmap> BitmapFromIcon(HICON icon);
    static DWORD WINAPI WorkerMain(void* parameter);
    static bool NeedsExactIcon(const std::wstring& name, bool is_dir,
                               const std::wstring& path);

    ID2D1DeviceContext* dc_ = nullptr;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    std::unordered_map<int, IImageList*> image_lists_;
    ComPtr<IWICImagingFactory> wic_;
    std::unordered_map<uint64_t, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::shared_ptr<WorkerState> worker_;
};

} // namespace pulse::ui
