// app_worker.cpp
#include "app_worker.h"
#include "app_model.h"
#include "entry_sort.h"
#include "link_resolve.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_net_cache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace pulse::app {

namespace {

std::wstring WorkKey(const std::wstring& path, ui::SortColumn col,
                     ui::SortDirection dir, FolderSortMode folders, bool preserve_order = false) {
    std::wstring key = path;
    key.push_back(L'\x1f');
    key += std::to_wstring(static_cast<int>(col));
    key.push_back(L':');
    key += std::to_wstring(static_cast<int>(dir));
    key.push_back(L':');
    key += std::to_wstring(static_cast<int>(folders));
    key.push_back(L':');
    key += preserve_order ? L"1" : L"0";
    return key;
}

} // namespace

namespace {
std::atomic<unsigned> g_live_workers{0};
constexpr unsigned kMaxLiveWorkers = 8;
}

struct WorkerPool::State {
    ResultCallback callback;
    std::mutex callback_mutex;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable finished;
    std::queue<WorkItem> queue;
    struct IoTask { std::function<void()> run; std::function<void()> complete; };
    std::queue<IoTask> io_queue;
    std::atomic<bool> running{false};
    unsigned live = 0;
    std::unordered_map<std::wstring, uint64_t> current_gen;
#ifdef PULSE_WORKER_TESTING
    std::function<void()> cancellation_checkpoint;
    std::function<void()> cache_write;
#endif
};

WorkerPool::WorkerPool() = default;

WorkerPool::~WorkerPool() {
    Stop();
}

void WorkerPool::Start(ResultCallback cb) {
    Stop();
    auto state = std::make_shared<State>();
    state->callback = std::move(cb);
    state->running = true;
    const unsigned count = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
    state->threads.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        unsigned live = g_live_workers.load();
        while (live < kMaxLiveWorkers && !g_live_workers.compare_exchange_weak(live, live + 1)) {}
        if (live >= kMaxLiveWorkers) break;
        {
            std::lock_guard lock(state->mutex);
            ++state->live;
        }
        try { state->threads.emplace_back(&WorkerPool::WorkerThread, state); }
        catch (...) {
            std::lock_guard lock(state->mutex);
            --state->live;
            --g_live_workers;
            break;
        }
    }
    if (state->threads.empty()) state->running = false;
    state_ = std::move(state);
}

void WorkerPool::Stop() {
    auto state = std::move(state_);
    if (!state) return;
    {
        std::lock_guard lock(state->callback_mutex);
        state->running = false;
        state->callback = {};
    }
    {
        std::lock_guard lock(state->mutex);
        state->queue = {};
        state->io_queue = {};
        state->current_gen.clear();
    }
    state->cv.notify_all();
    for (auto& thread : state->threads)
        if (thread.joinable()) CancelSynchronousIo(thread.native_handle());
    {
        std::unique_lock lock(state->mutex);
        state->finished.wait_for(lock, std::chrono::milliseconds(100), [&] { return state->live == 0; });
    }
    // Blocked provider calls retain only the mailbox, never the destroyed pool/UI.
    for (auto& thread : state->threads) if (thread.joinable()) thread.detach();
    state->threads.clear();
}

uint64_t WorkerPool::Refresh(const std::wstring& path, ui::SortColumn col,
                             ui::SortDirection dir, FolderSortMode folders) {
    const auto state = state_;
    if (!state || !state->running) return 0;
    const auto cache_write_ticket = fs::ReserveNetSnapshotWrite(path);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running) return 0;
    uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(path, col, dir, folders);
    state->current_gen[key] = gen;
    // Only supersede the same path+sort request. Separate panes may show the
    // same directory with different sort orders.
    std::queue<WorkItem> filtered;
    while (!state->queue.empty()) {
        if (state->queue.front().request_key != key) filtered.push(std::move(state->queue.front()));
        state->queue.pop();
    }
    state->queue = std::move(filtered);
    WorkItem item{ path, key, gen, col, dir };
    item.folder_sort = folders;
    item.cache_write_ticket = cache_write_ticket;
    state->queue.push(std::move(item));
    state->cv.notify_one();
    return gen;
}

void WorkerPool::Cancel(const std::wstring& path, uint64_t generation) {
    const auto state = state_;
    if (!state || !generation) return;
    std::lock_guard lock(state->mutex);
    for (auto it = state->current_gen.begin(); it != state->current_gen.end();) {
        // WorkKey's final separator always precedes its generated sort suffix;
        // matching the complete path also handles empty/virtual paths literally.
        const auto separator = it->first.find_last_of(L'\x1f');
        if (it->second == generation && separator != std::wstring::npos &&
            it->first.compare(0, separator, path) == 0)
            it = state->current_gen.erase(it);
        else ++it;
    }
    std::queue<WorkItem> pending;
    while (!state->queue.empty()) {
        auto& item = state->queue.front();
        if (item.path != path || item.generation != generation) pending.push(std::move(item));
        state->queue.pop();
    }
    state->queue = std::move(pending);
}

uint64_t WorkerPool::LoadPaths(const std::wstring& view_path,
                               std::vector<std::wstring> paths,
                               ui::SortColumn col, ui::SortDirection dir,
                               bool preserve_order,
                               std::vector<uint64_t> display_times, FolderSortMode folders) {
    const auto state = state_;
    if (!state || !state->running) return 0;
    const auto cache_write_ticket = fs::ReserveNetSnapshotWrite(view_path);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running) return 0;
    const uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(view_path, col, dir, folders, preserve_order);
    state->current_gen[key] = gen;
    std::queue<WorkItem> filtered;
    while (!state->queue.empty()) {
        if (state->queue.front().request_key != key) filtered.push(std::move(state->queue.front()));
        state->queue.pop();
    }
    state->queue = std::move(filtered);
    WorkItem item{ view_path, key, gen, col, dir };
    item.folder_sort = folders;
    item.cache_write_ticket = cache_write_ticket;
    item.load_paths = true;
    item.preserve_order = preserve_order;
    item.paths = std::move(paths);
    item.display_times = std::move(display_times);
    state->queue.push(std::move(item));
    state->cv.notify_one();
    return gen;
}

bool WorkerPool::EnqueueIo(std::function<void()> task, std::function<void()> completion) {
    const auto state = state_;
    if (!task || !state) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running || state->io_queue.size() >= 128) return false;
    state->io_queue.push({std::move(task), std::move(completion)});
    state->cv.notify_one();
    return true;
}

bool WorkerPool::IsCancelled(const std::shared_ptr<State>& state, const WorkItem& item) {
#ifdef PULSE_WORKER_TESTING
    if (state->cancellation_checkpoint) state->cancellation_checkpoint();
#endif
    if (!state->running) return true;
    std::lock_guard lock(state->mutex);
    const auto it = state->current_gen.find(item.request_key);
    return !state->running || it == state->current_gen.end() || it->second != item.generation;
}

WorkResult WorkerPool::Process(const std::shared_ptr<State>& state, const WorkItem& item) {
    WorkResult res;
    res.sort_column = item.sort_column;
    res.sort_direction = item.sort_direction;
    res.folder_sort = item.folder_sort;
    res.preserve_order = item.preserve_order;
    res.path = item.path;
    res.generation = item.generation;
    const auto cancelled = [&] { return IsCancelled(state, item); };
    if (cancelled()) { res.cancelled = true; return res; }
    if (!fs::IsVirtualPath(item.path) && !fs::IsUncPath(item.path))
        res.git_root = FindGitRoot(item.path);
    if (cancelled()) { res.cancelled = true; return res; }

    auto t0 = std::chrono::steady_clock::now();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    if (item.load_paths) {
        entries->reserve(item.paths.size());
        for (size_t i = 0; i < item.paths.size(); ++i) {
            if (cancelled()) { res.cancelled = true; return res; }
            const std::wstring& full = item.paths[i];
            if (full.empty() || fs::IsVirtualPath(full)) continue;
            fs::DirEntry entry;
            entry.full_path = full;
            std::wstring leaf = full;
            if (leaf.starts_with(L"\\\\?\\UNC\\")) leaf = L"\\\\" + leaf.substr(8);
            else if (leaf.starts_with(L"\\\\?\\")) leaf = leaf.substr(4);
            while (leaf.size() > 1 && (leaf.back() == L'\\' || leaf.back() == L'/')) leaf.pop_back();
            const auto slash = leaf.find_last_of(L"\\/");
            entry.name = slash == std::wstring::npos ? leaf : leaf.substr(slash + 1);
            WIN32_FILE_ATTRIBUTE_DATA data{};
            bool ok = GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data) != 0;
            if (cancelled()) { res.cancelled = true; return res; }
            if (!ok && leaf != full)
                ok = GetFileAttributesExW(leaf.c_str(), GetFileExInfoStandard, &data) != 0;
            if (cancelled()) { res.cancelled = true; return res; }
            if (ok) {
                entry.attrs = data.dwFileAttributes;
                entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                entry.cloud_recall =
                    (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
                             data.nFileSizeLow;
                entry.mtime = data.ftLastWriteTime;
            }
            if (i < item.display_times.size() && item.display_times[i] != 0) {
                entry.mtime.dwLowDateTime = static_cast<DWORD>(item.display_times[i]);
                entry.mtime.dwHighDateTime = static_cast<DWORD>(item.display_times[i] >> 32);
            }
            entries->push_back(std::move(entry));
        }
    } else if (fs::IsRecycleViewPath(item.path)) {
        try {
            fs::EnumerateRecycleBin(*entries, &res.recycle_info);
        } catch (...) {
            res.cancelled = cancelled();
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    } else {
        try {
            if (!fs::EnumerateDirectory(item.path, *entries, cancelled)) {
                res.cancelled = true;
                return res;
            }
        } catch (...) {
            res.cancelled = cancelled();
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    res.enum_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Check cancellation before sort.
    if (cancelled()) { res.cancelled = true; return res; }

    // Resolve .lnk targets (Recent folder, desktop shortcuts) before display.
    if (!fs::IsRecycleViewPath(item.path)) {
        ResolveLinksInPlace(item.path, *entries, cancelled);
    }
    if (cancelled()) { res.cancelled = true; return res; }

    auto t2 = std::chrono::steady_clock::now();
    // Interruptible sort: check every 8192 comparisons roughly via chunking.
    // For simplicity do full sort here; generation check after.
    size_t comparisons = 0;
    struct SortCancelled {};
    if (!item.preserve_order) {
        try {
            std::sort(entries->begin(), entries->end(),
                [&](const fs::DirEntry& a, const fs::DirEntry& b) {
                    if ((++comparisons & 8191u) == 0) {
                        if (cancelled()) throw SortCancelled{};
                    }
                    return EntryLess(a, b, item.sort_column, item.sort_direction, item.folder_sort, item.path);
                });
        } catch (const SortCancelled&) {
            res.cancelled = true;
            return res;
        }
    }
    auto t3 = std::chrono::steady_clock::now();
    res.sort_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    if (cancelled()) { res.cancelled = true; return res; }

    fs::QueryDirectoryIdentity(item.path, res.identity);
    if (cancelled()) { res.cancelled = true; return res; }
    res.snapshot = std::move(entries);

    // Timing output for large directories (visible in a debugger or ETW).
    if (res.snapshot && res.snapshot->size() >= 10000) {
        wchar_t msg[256];
        swprintf_s(msg, L"[Pulse] %s: %zu items, enum=%.2f ms sort=%.2f ms\n",
            item.path.c_str(), res.snapshot->size(), res.enum_ms, res.sort_ms);
        OutputDebugStringW(msg);
    }

    return res;
}

void WorkerPool::Publish(const std::shared_ptr<State>& state, const WorkItem& item, WorkResult res) {
    if (res.cancelled || IsCancelled(state, item)) return;
    const std::wstring cache_path = res.path;
    const fs::SnapshotPtr cache_snapshot = res.snapshot;
    {
        std::lock_guard lock(state->callback_mutex);
        if (!IsCancelled(state, item) && state->callback) {
            try { state->callback(std::move(res)); } catch (...) {}
        }
    }
    if (!cache_snapshot || !fs::IsUncPath(cache_path) || IsCancelled(state, item)) return;
#ifdef PULSE_WORKER_TESTING
    if (state->cache_write) { state->cache_write(); return; }
#endif
    fs::SaveNetSnapshot(cache_path, cache_snapshot, [&] { return IsCancelled(state, item); }, item.cache_write_ticket);
}

void WorkerPool::WorkerThread(std::shared_ptr<State> state) {
    struct Finish {
        std::shared_ptr<State> state;
        ~Finish() {
            { std::lock_guard lock(state->mutex); --state->live; }
            --g_live_workers;
            state->finished.notify_all();
        }
    } finish{state};
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComDone { HRESULT hr; ~ComDone() { if (SUCCEEDED(hr)) CoUninitialize(); } } com_done{com};
    while (state->running) {
        WorkItem item;
        State::IoTask io_task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] {
                return !state->queue.empty() || !state->io_queue.empty() || !state->running;
            });
            if (!state->running) return;
            if (!state->queue.empty()) {
                item = std::move(state->queue.front());
                state->queue.pop();
            } else if (!state->io_queue.empty()) {
                io_task = std::move(state->io_queue.front());
                state->io_queue.pop();
            } else {
                continue;
            }
        }
        if (io_task.run) {
            try { io_task.run(); } catch (...) {}
            std::lock_guard lock(state->callback_mutex);
            if (state->running && io_task.complete) {
                try { io_task.complete(); } catch (...) {}
            }
            continue;
        }
        WorkResult res;
        try { res = Process(state, item); }
        catch (...) { res.path = item.path; res.generation = item.generation; res.error = true; }
        Publish(state, item, std::move(res));
        // Completed generations no longer participate in cancellation checks.
        // Remove only when no newer request replaced this key while we were
        // processing, so a concurrent refresh remains authoritative.
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto it = state->current_gen.find(item.request_key);
            if (it != state->current_gen.end() && it->second == item.generation)
                state->current_gen.erase(it);
        }
    }
}

#ifdef PULSE_WORKER_TESTING
WorkResult WorkerCancellationTestProbe::Process(WorkItem item, size_t cancel_check, size_t* checks) {
    item.request_key = WorkKey(item.path, item.sort_column, item.sort_direction,
                               item.folder_sort, item.preserve_order);
    auto state = std::make_shared<WorkerPool::State>();
    state->running = true;
    state->current_gen[item.request_key] = item.generation;
    size_t count = 0;
    std::mutex gate;
    std::condition_variable changed;
    bool reached = false, release = false, finished = false;
    state->cancellation_checkpoint = [&] {
        ++count;
        if (cancel_check && count == cancel_check) {
            std::unique_lock lock(gate);
            reached = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }
    };
    WorkResult result;
    if (!cancel_check) result = WorkerPool::Process(state, item);
    else {
        WorkerPool pool;
        pool.state_ = state;
        std::thread worker([&] {
            try { result = WorkerPool::Process(state, item); }
            catch (...) { result.error = true; }
            { std::lock_guard lock(gate); finished = true; }
            changed.notify_all();
        });
        {
            std::unique_lock lock(gate);
            changed.wait_for(lock, std::chrono::seconds(5), [&] { return reached || finished; });
        }
        const std::wstring other_path = L"pulse:another-pane";
        const auto other_key = WorkKey(other_path, item.sort_column, item.sort_direction, item.folder_sort);
        { std::lock_guard lock(state->mutex); state->current_gen[other_key] = item.generation + 1; }
        pool.Cancel(item.path, item.generation);
        { std::lock_guard lock(gate); release = true; }
        changed.notify_all();
        worker.join();
        if (state->current_gen.find(other_key) == state->current_gen.end())
            throw std::runtime_error("Cancel removed the other pane's generation");
    }
    *checks = count;
    return result;
}

bool WorkerCancellationTestProbe::CancelPreservesOtherRequests() {
    WorkerPool pool;
    auto state = std::make_shared<WorkerPool::State>();
    state->running = true;
    pool.state_ = state;
    const std::wstring path = L"pulse:literal\npath\x1f";
    WorkItem old{path, WorkKey(path, ui::SortColumn::Name, ui::SortDirection::Asc,
                             FolderSortMode::FoldersFirst), 1, ui::SortColumn::Name, ui::SortDirection::Asc};
    WorkItem other{L"", WorkKey(L"", ui::SortColumn::Name, ui::SortDirection::Asc,
                               FolderSortMode::FoldersFirst), 1, ui::SortColumn::Name, ui::SortDirection::Asc};
    WorkItem newer{path, WorkKey(path, ui::SortColumn::Size, ui::SortDirection::Asc,
                               FolderSortMode::FoldersFirst), 2, ui::SortColumn::Size, ui::SortDirection::Asc};
    state->current_gen[old.request_key] = old.generation;
    state->current_gen[other.request_key] = other.generation;
    state->current_gen[newer.request_key] = newer.generation;
    state->queue.push(old); state->queue.push(other); state->queue.push(newer);
    pool.Cancel(path, old.generation);
    return !state->current_gen.contains(old.request_key) &&
        state->current_gen.contains(other.request_key) && state->current_gen.contains(newer.request_key) &&
        state->queue.size() == 2 && state->queue.front().path.empty() && state->queue.back().generation == 2;
}

void WorkerCancellationTestProbe::Publish(WorkResult result, bool stale, bool replace_in_callback,
                                         size_t* callbacks, size_t* cache_writes) {
    auto state = std::make_shared<WorkerPool::State>();
    state->running = true;
    const WorkItem item{result.path, L"publication", result.generation,
                        result.sort_column, result.sort_direction};
    state->current_gen[item.request_key] = item.generation + (stale ? 1 : 0);
    state->callback = [&, raw = state.get()](WorkResult) {
        ++*callbacks;
        if (replace_in_callback) {
            std::lock_guard lock(raw->mutex);
            raw->current_gen[item.request_key] = item.generation + 1;
        }
    };
    state->cache_write = [&] { ++*cache_writes; };
    WorkerPool::Publish(state, item, std::move(result));
}
#endif

} // namespace pulse::app
