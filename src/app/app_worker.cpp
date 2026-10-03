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
                     ui::SortDirection dir) {
    std::wstring key = path;
    key.push_back(L'\x1f');
    key += std::to_wstring(static_cast<int>(col));
    key.push_back(L':');
    key += std::to_wstring(static_cast<int>(dir));
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
                             ui::SortDirection dir) {
    const auto state = state_;
    if (!state || !state->running) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(path, col, dir);
    state->current_gen[key] = gen;
    // Only supersede the same path+sort request. Separate panes may show the
    // same directory with different sort orders.
    std::queue<WorkItem> filtered;
    while (!state->queue.empty()) {
        if (state->queue.front().request_key != key) filtered.push(std::move(state->queue.front()));
        state->queue.pop();
    }
    state->queue = std::move(filtered);
    state->queue.push(WorkItem{ path, key, gen, col, dir });
    state->cv.notify_one();
    return gen;
}

uint64_t WorkerPool::LoadPaths(const std::wstring& view_path,
                               std::vector<std::wstring> paths,
                               ui::SortColumn col, ui::SortDirection dir,
                               bool preserve_order,
                               std::vector<uint64_t> display_times) {
    const auto state = state_;
    if (!state || !state->running) return 0;
    std::lock_guard<std::mutex> lock(state->mutex);
    const uint64_t gen = ++global_gen_;
    const std::wstring key = WorkKey(view_path, col, dir);
    state->current_gen[key] = gen;
    std::queue<WorkItem> filtered;
    while (!state->queue.empty()) {
        if (state->queue.front().request_key != key) filtered.push(std::move(state->queue.front()));
        state->queue.pop();
    }
    state->queue = std::move(filtered);
    WorkItem item{ view_path, key, gen, col, dir };
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

WorkResult WorkerPool::Process(const std::shared_ptr<State>& state, const WorkItem& item) {
    WorkResult res;
    res.path = item.path;
    res.generation = item.generation;
    if (!fs::IsVirtualPath(item.path) && !fs::IsUncPath(item.path))
        res.git_root = FindGitRoot(item.path);

    auto t0 = std::chrono::steady_clock::now();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    if (item.load_paths) {
        entries->reserve(item.paths.size());
        for (size_t i = 0; i < item.paths.size(); ++i) {
            if ((i & 127u) == 0) {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto it = state->current_gen.find(item.request_key);
                if (!state->running || it == state->current_gen.end() || it->second != item.generation) {
                    res.cancelled = true;
                    return res;
                }
            }
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
            if (!ok && leaf != full)
                ok = GetFileAttributesExW(leaf.c_str(), GetFileExInfoStandard, &data) != 0;
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
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    } else {
        try {
            fs::EnumerateDirectory(item.path, *entries);
        } catch (...) {
            res.error = true;
            res.snapshot = nullptr;
            return res;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    res.enum_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Check cancellation before sort.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->current_gen.find(item.request_key);
        if (it == state->current_gen.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    // Resolve .lnk targets (Recent folder, desktop shortcuts) before display.
    if (!fs::IsRecycleViewPath(item.path)) {
        ResolveLinksInPlace(item.path, *entries, [&] {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto it = state->current_gen.find(item.request_key);
            return !state->running || it == state->current_gen.end() || it->second != item.generation;
        });
    }

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
                        std::lock_guard<std::mutex> lock(state->mutex);
                        const auto it = state->current_gen.find(item.request_key);
                        if (!state->running || it == state->current_gen.end() || it->second != item.generation)
                            throw SortCancelled{};
                    }
                    return EntryLess(a, b, item.sort_column, item.sort_direction, item.path);
                });
        } catch (const SortCancelled&) {
            res.cancelled = true;
            return res;
        }
    }
    auto t3 = std::chrono::steady_clock::now();
    res.sort_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        auto it = state->current_gen.find(item.request_key);
        if (it == state->current_gen.end() || it->second != item.generation) {
            res.cancelled = true;
            return res;
        }
    }

    res.snapshot = std::move(entries);
    fs::QueryDirectoryIdentity(item.path, res.identity);

    // Timing output for large directories (visible in a debugger or ETW).
    if (res.snapshot && res.snapshot->size() >= 10000) {
        wchar_t msg[256];
        swprintf_s(msg, L"[Pulse] %s: %zu items, enum=%.2f ms sort=%.2f ms\n",
            item.path.c_str(), res.snapshot->size(), res.enum_ms, res.sort_ms);
        OutputDebugStringW(msg);
    }

    return res;
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
        if (!res.cancelled) {
            const std::wstring cache_path = res.path;
            fs::SnapshotPtr cache_snapshot = res.snapshot;
            {
                std::lock_guard lock(state->callback_mutex);
                if (state->running && state->callback) {
                    try { state->callback(std::move(res)); } catch (...) {}
                }
            }
            if (state->running && cache_snapshot && fs::IsUncPath(cache_path))
                fs::SaveNetSnapshot(cache_path, cache_snapshot);
        }
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

} // namespace pulse::app
