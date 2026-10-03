// fs_watch.cpp
#include "fs_watch.h"
#include "fs_enum.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>

namespace pulse::fs {
namespace {

constexpr uint32_t kWatchLimit = 64;
struct WatchSlot {
    std::shared_ptr<void> owner;
    HANDLE thread = nullptr;
    bool retired = false;
};
struct WatchPool {
    std::mutex mutex;
    std::array<WatchSlot, kWatchLimit> slots;
    void ReapLocked() {
        for (auto& slot : slots) {
            if (slot.retired && slot.thread && WaitForSingleObject(slot.thread, 0) == WAIT_OBJECT_0)
                slot = {};
        }
    }
};
WatchPool& Watches() {
    // A filesystem driver may never complete cancellation. Keep the registry
    // alive through process exit rather than free its outstanding IO storage.
    static auto* pool = new WatchPool;
    return *pool;
}
#ifdef PULSE_FS_WATCH_TESTING
std::mutex g_hook_mutex;
std::function<void(DirWatch::TestPoint)> g_io_hook;
#endif

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE Get() const { return handle_; }
    HANDLE Release() {
        const HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void Reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_;
};

std::vector<DirNotifyEvent> ParseNotifyBuffer(const BYTE* data, DWORD bytes,
                                             std::wstring& pending_old, bool emit_old) {
    std::vector<DirNotifyEvent> out;
    if (!data || bytes < sizeof(FILE_NOTIFY_INFORMATION)) return out;
    const BYTE* p = data;
    const BYTE* end = data + bytes;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const size_t name_chars = info->FileNameLength / sizeof(WCHAR);
        const BYTE* name_end = reinterpret_cast<const BYTE*>(info->FileName) + info->FileNameLength;
        if (name_end > end) break;
        std::wstring name(info->FileName, name_chars);
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            pending_old = std::move(name);
            if (emit_old) out.push_back({FILE_ACTION_RENAMED_OLD_NAME, pending_old, {}});
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            DirNotifyEvent event;
            event.action = FILE_ACTION_RENAMED_NEW_NAME;
            event.name = std::move(name);
            event.old_name = std::move(pending_old);
            pending_old.clear();
            out.push_back(std::move(event));
        } else {
            out.push_back({info->Action, std::move(name), {}});
        }
        if (info->NextEntryOffset == 0) break;
        if (info->NextEntryOffset > static_cast<size_t>(end - p)) break;
        p += info->NextEntryOffset;
    }
    return out;
}

} // namespace

struct DirWatch::WorkerState {
    // All handles, OVERLAPPED and its destination buffer have the same owner.
    // No UI-side close/cancel races a worker's handle reuse.
    UniqueHandle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    UniqueHandle directory{INVALID_HANDLE_VALUE};
    UniqueHandle completion{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE thread = nullptr;
    uint32_t slot = 0;
    std::wstring path;
    bool subtree = false;
    std::atomic<bool> stop{false};
    std::atomic<bool> armed{false};
    std::mutex callback_mutex;
    ChangeCallback callback;
    OVERLAPPED overlapped{};
    BY_HANDLE_FILE_INFORMATION identity{};
    bool identity_valid = false;
    bool pending_io = false;
    std::wstring pending_rename_old;
    std::array<DWORD, 64 * 1024 / sizeof(DWORD)> buffer{};
#ifdef PULSE_FS_WATCH_TESTING
    std::function<void(TestPoint)> io_hook;
#endif

    ~WorkerState() {
        assert(!pending_io);
        if (thread) CloseHandle(thread);
    }

    HANDLE OpenDirectory(bool probe = false) const {
#ifdef PULSE_FS_WATCH_TESTING
        if (io_hook) io_hook(probe ? TestPoint::ProbePath : TestPoint::OpenDirectory);
#else
        (void)probe;
#endif
        if (stop.load(std::memory_order_acquire)) return INVALID_HANDLE_VALUE;
        return CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    }

    static bool ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& value) {
        return handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &value) != FALSE;
    }

    bool WatchedPathReplaced() const {
        if (!identity_valid) return false;
        UniqueHandle current(OpenDirectory(true));
        if (stop.load(std::memory_order_acquire)) return false;
        if (current.Get() == INVALID_HANDLE_VALUE) return true;
        BY_HANDLE_FILE_INFORMATION value{};
        if (!ReadIdentity(current.Get(), value)) return true;
        return value.dwVolumeSerialNumber != identity.dwVolumeSerialNumber ||
               value.nFileIndexHigh != identity.nFileIndexHigh ||
               value.nFileIndexLow != identity.nFileIndexLow;
    }

    bool ReopenDirectory() {
        assert(!pending_io);
        armed.store(false, std::memory_order_release);
        directory.Reset(INVALID_HANDLE_VALUE);
        identity_valid = false;
        while (!stop.load(std::memory_order_acquire)) {
            UniqueHandle opened(OpenDirectory());
            if (stop.load(std::memory_order_acquire)) return false;
            if (opened.Get() != INVALID_HANDLE_VALUE && ReadIdentity(opened.Get(), identity)) {
                directory.Reset(opened.Release());
                identity_valid = true;
                armed.store(true, std::memory_order_release);
                return true;
            }
            if (WaitForSingleObject(stop_event.Get(), 1000) == WAIT_OBJECT_0) break;
        }
        return false;
    }

    void Notify(bool overflow, std::vector<DirNotifyEvent> events) {
        // Stop takes this gate, suppresses new callbacks, and drains a callback
        // already running before the caller may destroy its captured objects.
        // No filesystem operation or IO wait is permitted under this gate.
        std::lock_guard lock(callback_mutex);
        if (stop.load(std::memory_order_acquire) || !callback) return;
        try {
            callback(overflow, std::move(events));
        } catch (...) {
        }
    }

    bool FinishRead(bool cancel, DWORD& transferred, DWORD& error) {
        if (cancel) {
            CancelIoEx(directory.Get(), &overlapped);
#ifdef PULSE_FS_WATCH_TESTING
            if (io_hook) io_hook(TestPoint::CancelCompletion);
#endif
        }
        // Cancellation is only a request. Keep every IO-owned byte alive until
        // the kernel confirms completion, even if that means bounded retirement.
        for (;;) {
            const BOOL got = GetOverlappedResult(directory.Get(), &overlapped, &transferred, TRUE);
            error = got ? ERROR_SUCCESS : GetLastError();
            if (got || error != ERROR_IO_INCOMPLETE) {
                pending_io = false;
                return got != FALSE;
            }
            // Do not reinterpret incomplete as cancelled and free OVERLAPPED.
            WaitForSingleObject(completion.Get(), INFINITE);
        }
    }

    void Run() {
        if (!ReopenDirectory()) return;
        bool newly_armed = true;
        while (!stop.load(std::memory_order_acquire)) {
            ResetEvent(completion.Get());
            overlapped = {};
            overlapped.hEvent = completion.Get();
            const BOOL ok = ReadDirectoryChangesW(directory.Get(), buffer.data(),
                static_cast<DWORD>(sizeof(buffer)), subtree ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                    FILE_NOTIFY_CHANGE_SECURITY, nullptr, &overlapped, nullptr);
            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                if (stop.load(std::memory_order_acquire)) break;
                pending_rename_old.clear();
                Notify(true, {});
                if (!ReopenDirectory()) break;
                newly_armed = true;
                continue;
            }
            pending_io = true;
            if (subtree && newly_armed) Notify(true, {});
            newly_armed = false;
            const HANDLE events[2] = {stop_event.Get(), completion.Get()};
            bool replaced = false;
            bool cancel = false;
            for (;;) {
                const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 500);
                if (wait == WAIT_TIMEOUT) {
                    if (!WatchedPathReplaced()) {
                        if (!stop.load(std::memory_order_acquire)) continue;
                    } else {
                        replaced = true;
                    }
                    cancel = true;
                    break;
                }
                cancel = wait != WAIT_OBJECT_0 + 1;
                break;
            }
            DWORD transferred = 0;
            DWORD error = ERROR_SUCCESS;
            const bool got = FinishRead(cancel, transferred, error);
            if (stop.load(std::memory_order_acquire)) break;
            if (replaced) {
                pending_rename_old.clear();
                Notify(true, {});
                if (!ReopenDirectory()) break;
                newly_armed = true;
                continue;
            }
            if (!got) {
                if (error == ERROR_OPERATION_ABORTED) continue;
                pending_rename_old.clear();
                Notify(true, {});
                if (error != ERROR_NOTIFY_ENUM_DIR && !ReopenDirectory()) break;
                newly_armed = true;
                continue;
            }
            if (transferred == 0) {
                pending_rename_old.clear();
                Notify(true, {});
            } else {
                Notify(false, ParseNotifyBuffer(reinterpret_cast<const BYTE*>(buffer.data()),
                    transferred, pending_rename_old, subtree));
            }
        }
        armed.store(false, std::memory_order_release);
        // No outstanding read remains here; FinishRead confirmed its completion.
        assert(!pending_io);
        directory.Reset(INVALID_HANDLE_VALUE);
    }
};

DirWatch::DirWatch() = default;
DirWatch::~DirWatch() { Stop(); }

#ifdef PULSE_FS_WATCH_TESTING
DirWatch::ResourceUsage DirWatch::Resources() {
    auto& pool = Watches();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    ResourceUsage usage{};
    usage.limit = kWatchLimit;
    for (const auto& slot : pool.slots) {
        if (slot.thread) ++usage.live;
        if (slot.retired) ++usage.retired;
    }
    return usage;
}
#endif

bool DirWatch::Start(const std::wstring& path, ChangeCallback callback, bool subtree) {
    Stop();
    if (path.empty()) return false;
    auto& pool = Watches();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    for (uint32_t i = 0; i < kWatchLimit; ++i) {
        if (pool.slots[i].thread) continue;
        auto state = std::make_shared<WorkerState>();
        if (!state->stop_event.Get() || !state->completion.Get()) return false;
        state->slot = i;
        state->path = NormalizePath(path);
        state->subtree = subtree;
        state->callback = std::move(callback);
#ifdef PULSE_FS_WATCH_TESTING
        {
            std::lock_guard hook_lock(g_hook_mutex);
            state->io_hook = g_io_hook;
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

void DirWatch::Stop() {
    const auto state = std::move(worker_);
    if (!state) return;
    state->stop.store(true, std::memory_order_release);
    state->armed.store(false, std::memory_order_release);
    SetEvent(state->stop_event.Get());
    {
        std::lock_guard lock(state->callback_mutex);
        state->callback = {};
    }
    // UI never calls CancelIoEx/CloseHandle on directory; either can be delayed
    // by the filesystem too. The worker cancels and drains its own IO.
    auto& pool = Watches();
    std::lock_guard lock(pool.mutex);
    pool.slots[state->slot].retired = true;
    pool.ReapLocked();
}

bool DirWatch::Armed() const {
    return worker_ && worker_->armed.load(std::memory_order_acquire);
}

DWORD WINAPI DirWatch::WorkerMain(void* parameter) {
    std::unique_ptr<std::shared_ptr<WorkerState>> argument(
        static_cast<std::shared_ptr<WorkerState>*>(parameter));
    const auto state = *argument;
    state->Run();
    state->armed.store(false, std::memory_order_release);
    return 0;
}

#ifdef PULSE_FS_WATCH_TESTING
void DirWatch::SetIoHookForTest(std::function<void(TestPoint)> hook) {
    std::lock_guard lock(g_hook_mutex);
    g_io_hook = std::move(hook);
}
#endif

DirWatchSet::DirWatchSet() : callbacks_(std::make_shared<CallbackState>()) {}
DirWatchSet::~DirWatchSet() { Stop(); }

void DirWatchSet::Sync(const std::vector<std::wstring>& paths, Callback callback) {
    {
        std::lock_guard lock(callbacks_->mutex);
        callbacks_->callback = std::move(callback);
    }
    std::vector<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.empty() || IsVirtualPath(path)) continue;
        wanted.push_back(NormalizePath(path));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    for (auto it = watches_.begin(); it != watches_.end();) {
        if (std::binary_search(wanted.begin(), wanted.end(), it->first)) ++it;
        else it = watches_.erase(it);
    }
    for (const auto& path : wanted) {
        if (watches_.contains(path)) continue;
        auto watch = std::make_unique<DirWatch>();
        const auto callbacks = callbacks_;
        if (!watch->Start(path, [callbacks, key = path](bool overflow, std::vector<DirNotifyEvent> events) {
            std::lock_guard lock(callbacks->mutex);
            if (callbacks->callback) callbacks->callback(key, overflow, std::move(events));
        })) continue;
        watches_[path] = std::move(watch);
    }
}

void DirWatchSet::Stop() {
    {
        std::lock_guard lock(callbacks_->mutex);
        callbacks_->callback = {};
    }
    watches_.clear();
}

bool DirWatchSet::Armed(const std::wstring& path) const {
    const auto it = watches_.find(NormalizePath(path));
    return it != watches_.end() && it->second && it->second->Armed();
}

} // namespace pulse::fs
