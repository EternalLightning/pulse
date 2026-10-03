// fs_watch.h — ReadDirectoryChangesW directory watcher.
// UNC opens, identity probes and cancellation completion run only in a worker.
#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::fs {

struct DirNotifyEvent {
    DWORD action = 0;
    std::wstring name;
    std::wstring old_name;
};

class DirWatch {
public:
    using ChangeCallback = std::function<void(bool overflow, std::vector<DirNotifyEvent> events)>;

    DirWatch();
    ~DirWatch();
    DirWatch(const DirWatch&) = delete;
    DirWatch& operator=(const DirWatch&) = delete;

    // Callbacks must be short, nonblocking and must not call this watch's Stop.
    // Stop synchronizes callbacks already entered, but never waits on filesystem
    // calls. After Stop returns no callback can still access its captures.
    bool Start(const std::wstring& path, ChangeCallback cb, bool subtree = false);
    void Stop();
    bool Armed() const;

#ifdef PULSE_FS_WATCH_TESTING
    struct ResourceUsage {
        uint32_t live = 0;
        uint32_t retired = 0;
        uint32_t limit = 0;
    };
    static ResourceUsage Resources();
    enum class TestPoint { OpenDirectory, ProbePath, CancelCompletion };
    // Snapshotted into worker-owned state. No test requires a UNC server.
    static void SetIoHookForTest(std::function<void(TestPoint)> hook);
#endif

private:
    struct WorkerState;
    static DWORD WINAPI WorkerMain(void* parameter);
    std::shared_ptr<WorkerState> worker_;
};

class DirWatchSet {
public:
    using Callback = std::function<void(const std::wstring& path, bool overflow,
                                        std::vector<DirNotifyEvent> events)>;
    DirWatchSet();
    ~DirWatchSet();
    DirWatchSet(const DirWatchSet&) = delete;
    DirWatchSet& operator=(const DirWatchSet&) = delete;

    // Same short/nonblocking callback contract as DirWatch; no callback may
    // reenter this set's Sync/Stop while its callback gate is held.
    void Sync(const std::vector<std::wstring>& paths, Callback cb);
    void Stop();
    bool Armed(const std::wstring& path) const;

private:
    struct CallbackState {
        std::mutex mutex;
        Callback callback;
    };
    std::shared_ptr<CallbackState> callbacks_;
    std::unordered_map<std::wstring, std::unique_ptr<DirWatch>> watches_;
};

} // namespace pulse::fs
