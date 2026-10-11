// app_worker.cpp
#include "app_worker.h"
#include "app_model.h"
#include "directory_projection.h"
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

struct DirectoryWork {
    std::vector<WorkItem> projections;
    bool closed = false;
};

struct WorkerPool::State {
    ResultCallback callback;
    std::mutex callback_mutex;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable finished;
    std::deque<WorkItem> queue;
    struct IoTask { std::function<void()> run; std::function<void()> complete; };
    std::queue<IoTask> io_queue;
    std::atomic<bool> running{false};
    unsigned live = 0;
    std::unordered_map<std::wstring, uint64_t> current_gen;
    std::unordered_map<std::wstring, std::vector<std::shared_ptr<DirectoryWork>>> directories;
    bool IsCurrent(const WorkItem& request) const {
        const auto current = current_gen.find(request.request_key);
        return current != current_gen.end() && current->second == request.generation;
    }
    bool HasCurrent(const DirectoryWork& group) const {
        return std::any_of(group.projections.begin(), group.projections.end(),
            [&](const WorkItem& request) { return IsCurrent(request); });
    }
    uint64_t CurrentProjection(const std::wstring& path, const std::wstring& key) const {
        const auto stored = directories.find(path);
        if (stored == directories.end()) return 0;
        for (auto group = stored->second.rbegin(); group != stored->second.rend(); ++group)
            for (const auto& request : (*group)->projections)
                if (!(*group)->closed && request.request_key == key && IsCurrent(request))
                    return request.generation;
        return 0;
    }
    std::shared_ptr<DirectoryWork> LatestDirectory(const std::wstring& path) const {
        const auto stored = directories.find(path);
        if (stored == directories.end()) return {};
        for (auto group = stored->second.rbegin(); group != stored->second.rend(); ++group)
            if (!(*group)->closed && HasCurrent(**group)) return *group;
        return {};
    }
    void ForgetDirectory(const std::wstring& path, const std::shared_ptr<DirectoryWork>& group) {
        const auto stored = directories.find(path);
        if (stored == directories.end()) return;
        std::erase(stored->second, group);
        if (stored->second.empty()) directories.erase(stored);
    }
    // Called with mutex held; inactive queued producers must not retain cached facts.
    void PruneQueuedDirectories() {
        std::erase_if(queue, [&](const WorkItem& item) {
            const auto& group = item.directory_work;
            if (!group) return false;
            std::erase_if(group->projections, [&](const WorkItem& request) { return !IsCurrent(request); });
            if (!group->closed && !group->projections.empty()) return false;
            group->closed = true;
            ForgetDirectory(item.path, group);
            return true;
        });
    }

#ifdef PULSE_WORKER_TESTING
    std::function<void()> cancellation_checkpoint;
    std::function<void()> cache_write;
    std::function<void()> facts_checkpoint;
    std::atomic<size_t>* enumerations = nullptr;
    std::atomic<size_t>* projections = nullptr;
#endif
};

WorkerPool::WorkerPool() = default;

WorkerPool::~WorkerPool() {
    Stop();
}

bool WorkerPool::Start(ResultCallback cb) {
    Stop();
    const unsigned desired = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
    // Reserve both lanes together; a partial pool cannot provide isolation.
    unsigned live = g_live_workers.load();
    unsigned count = 0;
    do {
        count = std::min(desired, kMaxLiveWorkers - live);
        if (count < 2) return false;
    } while (!g_live_workers.compare_exchange_weak(live, live + count));
    unsigned started = 0;
    try {
        auto state = std::make_shared<State>();
        state->callback = std::move(cb);
        state->threads.reserve(count);
        state->running = true;
        state_ = state;
        for (; started < count; ++started) {
            std::lock_guard lock(state->mutex);
            state->threads.emplace_back(&WorkerPool::WorkerThread, state, started == 0);
            ++state->live;
        }
        return true;
    } catch (...) {
        g_live_workers.fetch_sub(count - started);
        Stop();
        return false;
    }
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
        state->directories.clear();
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
                             ui::SortDirection dir, FolderSortMode folders, fs::SnapshotPtr cached) {
    const uint64_t gen = ++global_gen_;
    const auto state = state_;
    if (!state || !state->running) return 0;
    const auto cache_write_ticket = fs::ReserveNetSnapshotWrite(path);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running) return 0;
    const std::wstring key = WorkKey(path, col, dir, folders);
    // Only the producer carries the group pointer; projections cannot form a cycle.
    WorkItem item{ path, key, gen, col, dir };
    item.folder_sort = folders;
    item.cache_write_ticket = cache_write_ticket;
    item.cached_snapshot = std::move(cached);
    item.provider_request = fs::IsUncPath(path) || fs::IsRecycleViewPath(path);
    if (!fs::IsVirtualPath(path) && !fs::IsRecycleViewPath(path)) {
        std::shared_ptr<DirectoryWork> group;
        const auto previous = state->LatestDirectory(path);
        if (previous && std::none_of(previous->projections.begin(), previous->projections.end(),
                [&](const WorkItem& request) { return request.request_key == key; }))
            group = previous;
        // Repeating a sort key requests fresh filesystem facts. Other sorts can
        // finish the previous group, while this new producer accepts new sorts.
        state->current_gen[key] = gen;
        if (!group) {
            group = std::make_shared<DirectoryWork>();
            state->directories[path].push_back(group);
            WorkItem producer = item;
            producer.directory_work = group;
            producer.cached_snapshot.reset();
            producer.preserve_order = true;
            state->queue.push_back(std::move(producer));
        }
        group->projections.push_back(std::move(item));
    } else {
        state->current_gen[key] = gen;
        std::erase_if(state->queue, [&](const WorkItem& pending) {
            return !pending.directory_work && pending.request_key == key;
        });
        state->queue.push_back(std::move(item));
    }
    state->PruneQueuedDirectories();
    state->cv.notify_all();
    return gen;
}

uint64_t WorkerPool::Project(const std::wstring& path, fs::SnapshotPtr facts,
                              ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders) {
    const uint64_t gen = ++global_gen_;
    const auto state = state_;
    if (!facts || !state || !state->running) return 0;
    std::lock_guard lock(state->mutex);
    if (!state->running) return 0;
    const auto key = WorkKey(path, col, dir, folders);
    if (const auto shared = state->CurrentProjection(path, key)) return shared;
    const auto group = state->LatestDirectory(path);
    state->current_gen[key] = gen;
    std::erase_if(state->queue, [&](const WorkItem& pending) {
        return !pending.directory_work && pending.request_key == key;
    });
    WorkItem item{path, key, gen, col, dir};
    item.folder_sort = folders;
    if (group) {
        item.cached_snapshot = std::move(facts);
        std::erase_if(group->projections,
            [&](const WorkItem& request) { return request.request_key == key; });
        group->projections.push_back(std::move(item));
    } else {
        item.source_snapshot = std::move(facts);
        state->queue.push_back(std::move(item));
    }
    state->PruneQueuedDirectories();
    state->cv.notify_all();
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
    const auto groups = state->directories.find(path);
    if (groups != state->directories.end()) {
        std::erase_if(groups->second, [&](const auto& group) {
            if (state->HasCurrent(*group)) return false;
            group->closed = true;
            return true;
        });
        if (groups->second.empty()) state->directories.erase(groups);
    }
    state->PruneQueuedDirectories();
    std::deque<WorkItem> pending;
    while (!state->queue.empty()) {
        auto& item = state->queue.front();
        if (item.directory_work || item.path != path || item.generation != generation)
            pending.push_back(std::move(item));
        state->queue.pop_front();
    }
    state->queue = std::move(pending);
}

uint64_t WorkerPool::LoadPaths(const std::wstring& view_path,
                               std::vector<std::wstring> paths,
                               ui::SortColumn col, ui::SortDirection dir,
                               bool preserve_order,
                               std::vector<uint64_t> display_times, FolderSortMode folders) {
    const uint64_t gen = ++global_gen_;
    const auto state = state_;
    if (!state || !state->running) return 0;
    const auto cache_write_ticket = fs::ReserveNetSnapshotWrite(view_path);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running) return 0;
    const std::wstring key = WorkKey(view_path, col, dir, folders, preserve_order);
    state->current_gen[key] = gen;
    std::deque<WorkItem> filtered;
    while (!state->queue.empty()) {
        if (state->queue.front().directory_work || state->queue.front().request_key != key)
            filtered.push_back(std::move(state->queue.front()));
        state->queue.pop_front();
    }
    state->queue = std::move(filtered);
    WorkItem item{ view_path, key, gen, col, dir };
    item.folder_sort = folders;
    item.cache_write_ticket = cache_write_ticket;
    item.load_paths = true;
    item.preserve_order = preserve_order;
    item.paths = std::move(paths);
    item.display_times = std::move(display_times);
    item.provider_request = fs::IsUncPath(view_path) || fs::IsRecycleViewPath(view_path) ||
        std::any_of(item.paths.begin(), item.paths.end(), fs::IsUncPath);
    state->queue.push_back(std::move(item));
    state->PruneQueuedDirectories();
    state->cv.notify_all();
    return gen;
}

bool WorkerPool::EnqueueIo(std::function<void()> task, std::function<void()> completion) {
    const auto state = state_;
    if (!task || !state) return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->running || state->io_queue.size() >= 128) return false;
    state->io_queue.push({std::move(task), std::move(completion)});
    state->cv.notify_all();
    return true;
}

bool WorkerPool::IsCancelled(const std::shared_ptr<State>& state, const WorkItem& item) {
#ifdef PULSE_WORKER_TESTING
    if (state->cancellation_checkpoint) state->cancellation_checkpoint();
#endif
    if (!state->running) return true;
    std::lock_guard lock(state->mutex);
    if (item.directory_work) {
        const auto& group = item.directory_work;
        if (group->closed) return true;
        for (const auto& projection : group->projections) {
            const auto it = state->current_gen.find(projection.request_key);
            if (it != state->current_gen.end() && it->second == projection.generation) return false;
        }
        group->closed = true;
        state->ForgetDirectory(item.path, group);
        return true;
    }
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
    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }
    if (item.source_snapshot) {
        const auto start = std::chrono::steady_clock::now();
        res.facts = item.source_snapshot;
        res.projection_only = true;
#ifdef PULSE_WORKER_TESTING
        if (state->projections) ++*state->projections;
#endif
        res.snapshot = ProjectDirectorySnapshot(res.facts, item.path, item.sort_column,
            item.sort_direction, item.folder_sort, cancelled);
        res.sort_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        res.cancelled = !res.snapshot || cancelled();
        if (res.cancelled) res.snapshot.reset();
        return res;
    }
    if (!fs::IsVirtualPath(item.path) && !fs::IsUncPath(item.path))
        res.git_root = FindGitRoot(item.path);
    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }

    auto t0 = std::chrono::steady_clock::now();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    if (item.load_paths) {
        entries->reserve(item.paths.size());
        for (size_t i = 0; i < item.paths.size(); ++i) {
            if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }
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
            if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }
            if (!ok && leaf != full)
                ok = GetFileAttributesExW(leaf.c_str(), GetFileExInfoStandard, &data) != 0;
            if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }
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
            res.error_code = ERROR_GEN_FAILURE;
            res.snapshot = nullptr;
            return res;
        }
    } else {
        try {
#ifdef PULSE_WORKER_TESTING
            if (state->enumerations) ++*state->enumerations;
#endif
            if (!fs::EnumerateDirectory(item.path, *entries, cancelled)) {
                res.cancelled = true;
                return res;
            }
        } catch (const fs::EnumerationError& error) {
            res.cancelled = cancelled();
            res.error = true;
            res.error_code = error.Code();
            res.snapshot = nullptr;
            return res;
        } catch (...) {
            res.cancelled = cancelled();
            res.error = true;
            res.error_code = ERROR_GEN_FAILURE;
            res.snapshot = nullptr;
            return res;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    res.enum_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Check cancellation before sort.
    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }

    // Resolve .lnk targets (Recent folder, desktop shortcuts) before display.
    if (!fs::IsRecycleViewPath(item.path)) {
        ResolveLinksInPlace(item.path, *entries, cancelled);
    }
    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }

    auto t2 = std::chrono::steady_clock::now();
    res.facts = entries;
    if (item.preserve_order) res.snapshot = res.facts;
    else {
#ifdef PULSE_WORKER_TESTING
        if (state->projections) ++*state->projections;
#endif
        res.snapshot = ProjectDirectorySnapshot(res.facts, item.path, item.sort_column,
            item.sort_direction, item.folder_sort, cancelled);
        if (!res.snapshot) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }
    }
    auto t3 = std::chrono::steady_clock::now();
    res.sort_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }

    fs::QueryDirectoryIdentity(item.path, res.identity);
    if (cancelled()) { res.cancelled = true; res.snapshot.reset(); res.facts.reset(); return res; }

#ifdef PULSE_WORKER_TESTING
    if (item.directory_work && state->facts_checkpoint) state->facts_checkpoint();
#endif

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
    const bool final = res.final;
    {
        std::lock_guard lock(state->callback_mutex);
        if (!IsCancelled(state, item) && state->callback) {
            try { state->callback(std::move(res)); } catch (...) {}
        }
    }
    if (!final || item.source_snapshot || !cache_snapshot || !fs::IsUncPath(cache_path) || IsCancelled(state, item)) return;
#ifdef PULSE_WORKER_TESTING
    if (state->cache_write) { state->cache_write(); return; }
#endif
    fs::SaveNetSnapshot(cache_path, cache_snapshot, [&] { return IsCancelled(state, item); }, item.cache_write_ticket);
}

void WorkerPool::WorkerThread(std::shared_ptr<State> state, bool local_owner) {
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
            const auto request = [&] {
                return std::find_if(state->queue.begin(), state->queue.end(), [&](const WorkItem& queued) {
                    return !local_owner || !queued.provider_request;
                });
            };
            state->cv.wait(lock, [&] {
                return request() != state->queue.end() ||
                    (!local_owner && !state->io_queue.empty()) || !state->running;
            });
            if (!state->running) return;
            const auto queued = request();
            if (queued != state->queue.end()) {
                item = std::move(*queued);
                state->queue.erase(queued);
            } else if (!local_owner && !state->io_queue.empty()) {
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
        const auto process = [&](const WorkItem& request) {
            WorkResult result;
            result.path = request.path;
            result.generation = request.generation;
            result.sort_column = request.sort_column;
            result.sort_direction = request.sort_direction;
            result.folder_sort = request.folder_sort;
            result.preserve_order = request.preserve_order;
            try { result = Process(state, request); }
            catch (const fs::EnumerationError& error) {
                result.error = true;
                result.error_code = error.Code();
            } catch (...) {
                result.error = true;
                result.error_code = ERROR_GEN_FAILURE;
            }
            return result;
        };
        const auto provisional = [&](const WorkItem& request, const fs::SnapshotPtr& fallback = {},
                                     uint64_t cache_unix = 0) {
            const auto facts = request.cached_snapshot ? request.cached_snapshot : fallback;
            if (!facts) return;
            WorkItem projection = request;
            projection.directory_work.reset();
            projection.source_snapshot = facts;
            auto result = process(projection);
            result.final = false;
            result.cache_unix = request.cached_snapshot ? 0 : cache_unix;
            if (!result.error) Publish(state, request, std::move(result));
        };
        if (item.directory_work) {
            std::vector<WorkItem> cached;
            fs::SnapshotPtr fallback;
            bool load_disk = false;
            {
                std::lock_guard lock(state->mutex);
                cached = item.directory_work->projections;
                const auto memory = std::find_if(cached.begin(), cached.end(),
                    [&](const WorkItem& request) { return request.cached_snapshot && state->IsCurrent(request); });
                if (memory != cached.end()) fallback = memory->cached_snapshot;
                load_disk = !fallback && fs::IsUncPath(item.path) && std::any_of(cached.begin(), cached.end(),
                    [&](const WorkItem& request) { return !request.cached_snapshot && state->IsCurrent(request); });
            }
            uint64_t cache_unix = 0;
            if (load_disk) {
                // Optional disk facts share the existing request and precede its final enumeration.
                try { fallback = fs::LoadNetSnapshot(item.path, &cache_unix); } catch (...) {}
                std::lock_guard lock(state->mutex);
                cached = item.directory_work->projections;
            }
            for (const auto& request : cached) provisional(request, fallback, cache_unix);
        } else provisional(item);

        auto res = process(item);
        std::vector<WorkItem> projections;
        if (item.directory_work) {
            std::lock_guard lock(state->mutex);
            item.directory_work->closed = true;
            state->ForgetDirectory(item.path, item.directory_work);
            projections = std::move(item.directory_work->projections);
        } else projections.push_back(item);
        for (const auto& request : projections) {
            WorkResult result = res;
            if (item.directory_work && !res.cancelled && !res.error && res.snapshot) {
                WorkItem projection = request;
                projection.source_snapshot = res.snapshot;
                result = process(projection);
                result.projection_only = false;
                result.identity = res.identity;
                result.git_root = res.git_root;
                result.enum_ms = res.enum_ms;
            }
            result.path = request.path;
            result.generation = request.generation;
            result.sort_column = request.sort_column;
            result.sort_direction = request.sort_direction;
            result.folder_sort = request.folder_sort;
            result.preserve_order = request.preserve_order;
            Publish(state, request, std::move(result));
            // A replacement submitted while publishing still owns its generation.
            std::lock_guard lock(state->mutex);
            const auto it = state->current_gen.find(request.request_key);
            if (it != state->current_gen.end() && it->second == request.generation)
                state->current_gen.erase(it);
        }
    }
}

#ifdef PULSE_WORKER_TESTING
void WorkerCancellationTestProbe::SetCheckpoint(WorkerPool& pool, std::function<void()> checkpoint) {
    const auto state = pool.state_;
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->cancellation_checkpoint = std::move(checkpoint);
}

void WorkerCancellationTestProbe::SetFactsCheckpoint(WorkerPool& pool, std::function<void()> checkpoint) {
    const auto state = pool.state_;
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->facts_checkpoint = std::move(checkpoint);
}

size_t WorkerCancellationTestProbe::PendingDirectoryGroups(WorkerPool& pool) {
    const auto state = pool.state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return std::count_if(state->queue.begin(), state->queue.end(),
        [](const WorkItem& item) { return !!item.directory_work; });
}

size_t WorkerCancellationTestProbe::ProviderOwners(WorkerPool& pool) {
    const auto state = pool.state_;
    if (!state) return 0;
    std::lock_guard lock(state->mutex);
    return state->threads.size() - 1;
}

void WorkerCancellationTestProbe::SetCounters(WorkerPool& pool, std::atomic<size_t>* enumerations,
                                               std::atomic<size_t>* projections) {
    const auto state = pool.state_;
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->enumerations = enumerations;
    state->projections = projections;
}

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
    state->queue.push_back(old); state->queue.push_back(other); state->queue.push_back(newer);
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
