// app_worker.h — Async enumeration/sort worker with generation tracking.
#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_net_cache.h"
#include "entry_sort.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct WorkItem {
    std::wstring path;
    std::wstring request_key;
    uint64_t generation;
    ui::SortColumn sort_column;
    ui::SortDirection sort_direction;
    bool load_paths = false;
    bool preserve_order = false;
    std::vector<std::wstring> paths;
    std::vector<uint64_t> display_times;
    FolderSortMode folder_sort = FolderSortMode::FoldersFirst;
    fs::NetSnapshotWriteTicket cache_write_ticket;
};

struct WorkResult {
    std::wstring path;
    uint64_t generation;
    fs::SnapshotPtr snapshot;
    fs::DirectoryIdentity identity;
    std::wstring git_root;
    bool cancelled = false;
    bool error = false;
    double enum_ms = 0.0;
    double sort_ms = 0.0;
    fs::RecycleBinInfo recycle_info;
    ui::SortColumn sort_column = ui::SortColumn::Name;
    ui::SortDirection sort_direction = ui::SortDirection::Asc;
    FolderSortMode folder_sort = FolderSortMode::FoldersFirst;
    bool preserve_order = false;
};

using ResultCallback = std::function<void(WorkResult)>;

class WorkerPool {
public:
    WorkerPool();
    ~WorkerPool();

    void Start(ResultCallback cb);
    void Stop();
    // Cancel only this pane's old path/generation when navigation replaces it.
    void Cancel(const std::wstring& path, uint64_t generation);

    // Enqueue a refresh for path. Returns the generation assigned.
    uint64_t Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir,
                     FolderSortMode folders = FolderSortMode::FoldersFirst);

    uint64_t LoadPaths(const std::wstring& view_path, std::vector<std::wstring> paths,
                       ui::SortColumn col, ui::SortDirection dir,
                       bool preserve_order = false,
                       std::vector<uint64_t> display_times = {},
                       FolderSortMode folders = FolderSortMode::FoldersFirst);

    // Tasks must own their inputs; they cannot retain the pool or UI objects.
    bool EnqueueIo(std::function<void()> task, std::function<void()> completion = {});

    // Read on the UI thread, which also issues Refresh/LoadPaths requests.
    uint64_t ResultWatermark() const noexcept { return global_gen_; }

private:
    struct State;
    static bool IsCancelled(const std::shared_ptr<State>& state, const WorkItem& item);
    static void Publish(const std::shared_ptr<State>& state, const WorkItem& item, WorkResult result);
    static void WorkerThread(std::shared_ptr<State> state);
    static WorkResult Process(const std::shared_ptr<State>& state, const WorkItem& item);
    std::shared_ptr<State> state_;
    uint64_t global_gen_ = 0;
#ifdef PULSE_WORKER_TESTING
    friend struct WorkerCancellationTestProbe;
#endif
};

#ifdef PULSE_WORKER_TESTING
struct WorkerCancellationTestProbe {
    static WorkResult Process(WorkItem item, size_t cancel_check, size_t* checks);
    static bool CancelPreservesOtherRequests();
    static void Publish(WorkResult result, bool stale, bool replace_in_callback,
                        size_t* callbacks, size_t* cache_writes);
};
#endif

} // namespace pulse::app
