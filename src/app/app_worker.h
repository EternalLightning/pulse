// app_worker.h — Async enumeration/sort worker with generation tracking.
#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../ui/ui_renderer.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <functional>
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
};

using ResultCallback = std::function<void(WorkResult)>;

class WorkerPool {
public:
    WorkerPool();
    ~WorkerPool();

    void Start(ResultCallback cb);
    void Stop();

    // Enqueue a refresh for path. Returns the generation assigned.
    uint64_t Refresh(const std::wstring& path, ui::SortColumn col, ui::SortDirection dir);

    uint64_t LoadPaths(const std::wstring& view_path, std::vector<std::wstring> paths,
                       ui::SortColumn col, ui::SortDirection dir,
                       bool preserve_order = false,
                       std::vector<uint64_t> display_times = {});

    // Tasks must own their inputs; they cannot retain the pool or UI objects.
    bool EnqueueIo(std::function<void()> task, std::function<void()> completion = {});

private:
    struct State;
    static void WorkerThread(std::shared_ptr<State> state);
    static WorkResult Process(const std::shared_ptr<State>& state, const WorkItem& item);
    std::shared_ptr<State> state_;
    uint64_t global_gen_ = 0;
};

} // namespace pulse::app
