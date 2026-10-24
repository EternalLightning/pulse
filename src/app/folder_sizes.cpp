#include "folder_sizes.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <limits>
#include <set>
#include "../common/path_utils.h"
#include "../index/index_feed.h"

namespace pulse::app {
namespace {
constexpr size_t kCacheLimit = 4096;
constexpr ULONGLONG kFreshMs = 60000;
constexpr unsigned kWorkerLimit = 4;
std::atomic<unsigned> live_workers{0};
std::wstring Key(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    while (path.size() > 7 && path.back() == L'\\') path.pop_back();
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}
bool Within(const std::wstring& path, const std::wstring& parent) {
    return path.starts_with(parent) && (path.size() == parent.size() ||
        (!parent.empty() && parent.back() == L'\\') || path[parent.size()] == L'\\');
}
bool AddBytes(uint64_t& total, uint64_t bytes) {
    if (bytes > UINT64_MAX - total) return false;
    total += bytes;
    return true;
}
struct Scan {
    struct Frame {
        std::wstring path;
        std::unique_ptr<fs::DirectoryEnumerator> cursor;
        uint64_t bytes = 0;
        uint64_t files = 0, dirs = 0;
        bool started = false, partial = false, read_any = false;
    };
    std::wstring path;
    uint64_t epoch = 0;
    std::vector<Frame> stack;
    FolderSizeValue result{FolderSizeState::Calculating};
    bool done = false;
    explicit Scan(std::wstring p, uint64_t e) : path(std::move(p)), epoch(e) {
        stack.push_back(Frame{path});
    }
    void FinishFrame() {
        auto f = std::move(stack.back());
        stack.pop_back();
        if (stack.empty()) {
            done = true;
            result.bytes = f.bytes;
            result.has_value = f.read_any;
            result.files = f.files;
            result.dirs = f.dirs;
            result.counts_valid = f.read_any;
            result.incomplete = f.partial;
            result.state = !f.read_any ? FolderSizeState::Unavailable :
                (f.partial ? FolderSizeState::Partial : FolderSizeState::Ready);
        } else {
            auto& parent = stack.back();
            const bool added = AddBytes(parent.bytes, f.bytes);
            parent.partial |= f.partial || !added;
            parent.partial |= !AddBytes(parent.files, f.files) || !AddBytes(parent.dirs, f.dirs);
        }
    }
    // Round-robin slices keep a large first folder from starving its siblings.
    void Step(const std::atomic<bool>& stopping) {
        const auto deadline = GetTickCount64() + 12;
        unsigned processed = 0;
        while (!done && !stopping && GetTickCount64() < deadline && processed++ < 256) {
            auto& f = stack.back();
            if (!f.started) {
                f.started = true;
                const DWORD attrs = GetFileAttributesW(f.path.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    f.partial = true; FinishFrame(); continue;
                }
                // Never traverse junctions/symlinks or hydrate cloud directories.
                if (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) {
                    f.partial = true; FinishFrame(); continue;
                }
                f.cursor = std::make_unique<fs::DirectoryEnumerator>(f.path);
            }
            fs::DirEntry d;
            fs::DirectoryRead read;
            try {
                read = f.cursor->Next(d, [&] { return stopping.load(); });
            } catch (const fs::EnumerationError& error) {
                OutputDebugStringA((std::string("Pulse folder size: ") + error.what() + "\n").c_str());
                f.read_any |= f.cursor->ReadAny();
                f.partial = true;
                FinishFrame(); continue;
            }
            if (read == fs::DirectoryRead::Cancelled) return;
            f.read_any = true; // Successful empty enumeration is a known zero.
            if (read == fs::DirectoryRead::End) { FinishFrame(); continue; }
            if (d.is_dir) {
                f.partial |= !AddBytes(f.dirs, 1);
                if ((d.attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)) ||
                    stack.size() >= 128) { f.partial = true; continue; }
                const auto child = f.path + L"\\" + d.name;
                stack.push_back(Frame{child});
            } else {
                f.partial |= !AddBytes(f.files, 1);
                // Logical size includes cloud placeholders without opening content.
                f.partial |= !AddBytes(f.bytes, d.size);
            }
        }
    }
};
}

struct FolderSizes::Impl {
    struct Entry {
        FolderSizeValue value;
        ULONGLONG completed = 0, used = 0, not_before = 0;
        uint64_t epoch = 0;
    };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    bool worker_finished = false;
    std::map<std::wstring, Entry> cache;
    std::map<std::wstring, bool> wanted;
    std::set<std::wstring> manual, blocked_auto;
    std::set<std::wstring> require_counts;
    std::vector<std::wstring> roots;
    std::vector<FolderSizeRequest> last_requests;
    std::vector<std::wstring> last_roots;
    std::function<std::wstring()> cache_path;
    std::thread worker;
    std::atomic<bool> stopping{false}, changed{false};
    bool dirty = false;
    bool index_enabled = true;
    HANDLE index_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#ifdef PULSE_FOLDER_SIZES_TESTING
    std::function<void(const std::wstring&)> scan_hook;
    unsigned sync_revision = 0;
#endif
    ~Impl() { if (index_cancel) CloseHandle(index_cancel); }

    void Invalidate(const std::wstring& path) {
        const auto key = Key(path);
        std::lock_guard lock(mutex);
        for (auto& [p, e] : cache) if (Within(key, p) || Within(p, key)) {
            e.completed = 0; ++e.epoch;
            e.not_before = GetTickCount64() + 200;
            e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
        }
        changed = true;
        wake.notify_one();
    }
    void Load(const std::wstring& file) {
        std::wstring text;
        if (file.empty() || !ReadUtf8File(file, text)) return;
        std::lock_guard lock(mutex);
        for (const auto& record : json::ExtractStringArray(text, L"folders")) {
            const auto tab = record.find(L'\t');
            if (tab == std::wstring::npos || tab + 3 >= record.size() || record[tab + 2] != L'\t') continue;
            uint64_t bytes = 0;
            if (!json::ParseUnsigned(std::wstring_view(record).substr(0, tab), bytes) ||
                (record[tab + 1] != L'0' && record[tab + 1] != L'1')) continue;
            const auto path = Key(record.substr(tab + 3));
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            auto& e = cache[path];
            e.value = {FolderSizeState::Updating, bytes, true};
            e.value.incomplete = record[tab + 1] == L'1';
            if (cache.size() >= kCacheLimit) break;
        }
        changed = true;
    }
    void Save(const std::wstring& file) {
        if (file.empty()) return;
        std::wstring text = L"{\"folders\":[";
        {
            std::lock_guard lock(mutex);
            if (!dirty) return;
            bool first = true;
            for (const auto& [p, e] : cache) {
                if (!e.value.has_value) continue;
                if (!first) text += L",";
                first = false;
                text += L"\"";
                json::Escape(std::to_wstring(e.value.bytes) + L"\t" +
                    (e.value.incomplete ? L"1\t" : L"0\t") + p, text);
                text += L"\"";
            }
            dirty = false;
        }
        text += L"]}";
        WriteUtf8FileAtomic(file, text);
    }
    void Run() {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto file = cache_path ? cache_path() : std::wstring();
        const auto cache_key = file.empty() ? std::wstring() : Key(file);
        Load(file);
        std::map<std::wstring, std::unique_ptr<fs::DirWatch>> watches;
        std::map<std::wstring, UINT> drive_types;
        auto driveType = [&](const std::wstring& p) {
            const auto plain = path::StripExtendedPathPrefix(p);
            if (plain.size() < 3 || plain[1] != L':') return static_cast<UINT>(DRIVE_REMOTE);
            const auto root = plain.substr(0, 3);
            auto it = drive_types.find(root);
            if (it == drive_types.end()) it = drive_types.emplace(root, GetDriveTypeW(root.c_str())).first;
            return it->second;
        };
        std::deque<std::unique_ptr<Scan>> jobs;
        index::IndexFeedConnection connection(index_cancel);
        ULONGLONG next_index = 0;
        size_t index_cursor = 0;
        std::set<std::wstring> indexed_paths;
        ULONGLONG last_save = GetTickCount64();
        while (!stopping) {
            std::vector<std::wstring> desired_roots;
            std::vector<std::wstring> classify;
            {
                std::lock_guard lock(mutex);
                for (const auto& [p, automatic] : wanted) {
                    if (automatic && !manual.contains(p) && !require_counts.contains(p)) classify.push_back(p);
                }
            }
            for (const auto& p : classify) {
                const UINT type = driveType(p);
                if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) {
                    std::lock_guard lock(mutex);
                    blocked_auto.insert(p);
                    if (!manual.contains(p) && wanted.contains(p)) wanted[p] = false;
                    changed = true;
                }
            }
            if (index_enabled && !stopping && GetTickCount64() >= next_index) {
                std::vector<std::wstring> paths;
                std::vector<uint64_t> epochs;
                {
                    std::lock_guard lock(mutex);
                    if (!stopping) ResetEvent(index_cancel);
                    for (const auto& [p, automatic] : wanted) {
                        if (!automatic || manual.contains(p) || require_counts.contains(p) || fs::IsUncPath(p)) continue;
                        paths.push_back(p); epochs.push_back(cache[p].epoch);
                    }
                    std::erase_if(indexed_paths, [&](const auto& p) { return !wanted.contains(p); });
                    // Multiple panes can exceed one protocol batch. Rotate so
                    // every visible folder continues receiving index updates.
                    if (!paths.empty()) {
                        index_cursor %= paths.size();
                        std::rotate(paths.begin(), paths.begin() + index_cursor, paths.end());
                        std::rotate(epochs.begin(), epochs.begin() + index_cursor, epochs.end());
                        index_cursor = (index_cursor + index::kFolderSizeBatch) % paths.size();
                        if (paths.size() > index::kFolderSizeBatch) {
                            paths.resize(index::kFolderSizeBatch); epochs.resize(index::kFolderSizeBatch);
                        }
                    }
                }
                std::vector<index::IndexedFolderSize> values;
                const bool received = !paths.empty() && connection.FolderSizes(paths, values);
                next_index = GetTickCount64() + (received || paths.empty() ? 1000 : 10000);
                std::lock_guard lock(mutex);
                for (size_t i = 0; i < paths.size(); ++i) {
                    auto found = cache.find(paths[i]);
                    if (found == cache.end() || !wanted.contains(paths[i]) ||
                        found->second.epoch != epochs[i] || manual.contains(paths[i]) ||
                        require_counts.contains(paths[i])) continue;
                    auto& e = found->second;
                    if (received && values[i].available) {
                        if (e.value.state != FolderSizeState::Indexed || e.value.bytes != values[i].bytes) changed = true;
                        e.value = {FolderSizeState::Indexed, values[i].bytes, true};
                        e.completed = GetTickCount64();
                        indexed_paths.insert(paths[i]);
                        dirty = true;
                        std::erase_if(jobs, [&](const auto& job) { return job->path == paths[i]; });
                    } else if (indexed_paths.erase(paths[i]) || e.value.state == FolderSizeState::Indexed) {
                        e.completed = 0;
                        e.value.state = FolderSizeState::Updating;
                        changed = true;
                    }
                }
            }
            {
                std::unique_lock lock(mutex);
                std::erase_if(jobs, [&](const auto& j) {
                    const auto found = wanted.find(j->path);
                    return found == wanted.end() || !found->second || cache[j->path].epoch != j->epoch;
                });
                const auto now = GetTickCount64();
                for (const auto& [p, automatic] : wanted) {
                    if (!automatic) continue;
                    auto& e = cache[p];
                    if (indexed_paths.contains(p) && !manual.contains(p) && !require_counts.contains(p)) continue;
                    if (now < e.not_before) continue;
                    const bool watched = std::any_of(watches.begin(), watches.end(), [&](const auto& w) {
                        return w.second->Armed() && Within(p, w.first);
                    });
                    const bool needs_counts = require_counts.contains(p) && !e.value.counts_valid &&
                        e.value.state == FolderSizeState::Indexed;
                    if (e.completed && !needs_counts &&
                        (watched || now - e.completed < kFreshMs)) continue;
                    if (std::any_of(jobs.begin(), jobs.end(), [&](const auto& j) { return j->path == p; })) continue;
                    e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
                    if (jobs.size() >= 32) break;
                    jobs.push_back(std::make_unique<Scan>(p, e.epoch));
                    changed = true;
                }
                desired_roots = roots;
                if (jobs.empty()) wake.wait_for(lock, std::chrono::milliseconds(250));
            }
            std::erase_if(watches, [&](const auto& pair) {
                const bool remove = std::find(desired_roots.begin(), desired_roots.end(), pair.first) == desired_roots.end();
                if (remove) Invalidate(pair.first);
                return remove;
            });
            for (const auto& root : desired_roots) if (!watches.contains(root)) {
                if (driveType(root) == DRIVE_REMOTE) continue;
                Invalidate(root);
                auto watch = std::make_unique<fs::DirWatch>();
                watch->Start(root, [this, root, cache_key](bool overflow, std::vector<fs::DirNotifyEvent> events) {
                    if (overflow) Invalidate(root);
                    else for (const auto& event : events) {
                        const auto changed_path = Key(root + L"\\" + event.name);
                        // Do not create a scan/save/watch feedback loop in our data folder.
                        if (!cache_key.empty() && (changed_path == cache_key || changed_path == cache_key + L".tmp")) continue;
                        Invalidate(root + L"\\" + event.name);
                        if (!event.old_name.empty()) Invalidate(root + L"\\" + event.old_name);
                    }
                }, true);
                watches.emplace(root, std::move(watch));
            }
            if (!jobs.empty() && !stopping) {
                auto job = std::move(jobs.front()); jobs.pop_front();
#ifdef PULSE_FOLDER_SIZES_TESTING
                if (scan_hook) scan_hook(job->path);
#endif
                job->Step(stopping);
                if (job->done) {
                    std::lock_guard lock(mutex);
                    auto it = cache.find(job->path);
                    if (it != cache.end() && it->second.epoch == job->epoch && wanted.contains(job->path)) {
                        auto& e = it->second;
                        // A failed refresh retains the cached value and labels it incomplete.
                        if (!job->result.has_value && e.value.has_value) e.value.state = FolderSizeState::Cached;
                        else e.value = job->result;
                        e.completed = GetTickCount64();
                        dirty = true; changed = true;
                    }
                } else jobs.push_back(std::move(job));
                // Keep metadata enumeration from monopolizing a busy disk.
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(4), [&] { return stopping.load(); });
            }
            if (GetTickCount64() - last_save >= 3000) { Save(file); last_save = GetTickCount64(); }
        }
        watches.clear();
        Save(file);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};

FolderSizes::FolderSizes() : impl_(std::make_shared<Impl>()) {}
FolderSizes::~FolderSizes() { Stop(); }
void FolderSizes::SetCachePath(std::function<std::wstring()> path) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->cache_path = std::move(path);
}
void FolderSizes::SetIndexEnabled(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->index_enabled = enabled;
}
bool FolderSizes::NeedsWorker() const {
    std::lock_guard lock(impl_->mutex);
    return !impl_->stopping && !impl_->worker.joinable() &&
        !impl_->wanted.empty() && live_workers.load() < kWorkerLimit;
}
void FolderSizes::Sync(std::vector<FolderSizeRequest> visible, std::vector<std::wstring> roots) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    if (s.stopping) return;
    // A refused worker must still be retried when another window releases a
    // worker slot. Otherwise identical interests have no state to synchronize.
    if (s.worker.joinable() && visible == s.last_requests && roots == s.last_roots) return;
    s.last_requests = visible;
    s.last_roots = roots;
#ifdef PULSE_FOLDER_SIZES_TESTING
    ++s.sync_revision;
#endif
    std::map<std::wstring, bool> wanted;
    std::set<std::wstring> require_counts;
    for (const auto& request : visible) {
        const auto key = Key(request.path);
        wanted[key] |= (request.automatic && (!s.blocked_auto.contains(key) || request.require_counts)) || s.manual.contains(key);
        if (request.require_counts) require_counts.insert(key);
        s.cache[key].used = GetTickCount64();
    }
    bool cancelled = false;
    for (const auto& [p, automatic] : s.wanted) if (!wanted.contains(p)) {
        ++s.cache[p].epoch;
        s.manual.erase(p);
        cancelled |= automatic;
    }
    s.wanted = std::move(wanted);
    s.require_counts = std::move(require_counts);
    for (auto& root : roots) root = Key(root);
    std::sort(roots.begin(), roots.end()); roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    s.roots = std::move(roots);
    while (s.cache.size() > kCacheLimit) {
        auto oldest = s.cache.end();
        for (auto it = s.cache.begin(); it != s.cache.end(); ++it)
            if (!s.wanted.contains(it->first) && (oldest == s.cache.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest == s.cache.end()) break;
        s.cache.erase(oldest);
    }
    if (!s.worker.joinable() && !s.wanted.empty()) {
        unsigned live = live_workers.load();
        while (live < kWorkerLimit && !live_workers.compare_exchange_weak(live, live + 1)) {}
        if (live < kWorkerLimit) {
            const auto state = impl_;
            try {
                s.worker = std::thread([state] {
                    try { state->Run(); } catch (...) {
                        std::lock_guard lock(state->mutex);
                        for (const auto& [p, automatic] : state->wanted)
                            if (automatic) state->cache[p].value.state = state->cache[p].value.has_value
                                ? FolderSizeState::Cached : FolderSizeState::Unavailable;
                        state->changed = true;
                    }
                    {
                        std::lock_guard lock(state->mutex);
                        state->worker_finished = true;
                    }
                    --live_workers;
                    state->finished.notify_all();
                });
            } catch (...) { --live_workers; }
        }
        if (!s.worker.joinable()) {
            for (const auto& [p, automatic] : s.wanted)
                if (automatic) s.cache[p].value.state = s.cache[p].value.has_value
                    ? FolderSizeState::Cached : FolderSizeState::Unavailable;
            s.changed = true;
        }
    }
    if (cancelled && s.worker.joinable()) CancelSynchronousIo(s.worker.native_handle());
    if (cancelled && s.index_cancel) SetEvent(s.index_cancel);
    s.wake.notify_one();
}
void FolderSizes::Calculate(const std::wstring& path) {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    const auto key = Key(path);
    auto it = s.wanted.find(key);
    if (it == s.wanted.end()) return;
    it->second = true;
    s.manual.insert(key);
    auto& e = s.cache[key]; e.completed = 0; e.not_before = 0; ++e.epoch;
    e.value.state = e.value.has_value ? FolderSizeState::Updating : FolderSizeState::Calculating;
    s.changed = true; s.wake.notify_one();
}
FolderSizeValue FolderSizes::Get(const std::wstring& path) const {
    auto& s = *impl_;
    std::lock_guard lock(s.mutex);
    const auto key = Key(path);
    const auto it = s.cache.find(key);
    auto value = it == s.cache.end() ? FolderSizeValue{} : it->second.value;
    const auto wanted = s.wanted.find(key);
    if (wanted != s.wanted.end() && !wanted->second) {
        if (value.state == FolderSizeState::Calculating) value.state = FolderSizeState::Manual;
        if (value.state == FolderSizeState::Updating) value.state = FolderSizeState::Cached;
    }
    if (!value.has_value && value.state == FolderSizeState::Manual) {
        const auto request = s.wanted.find(key);
        if (request != s.wanted.end() && request->second) value.state = FolderSizeState::Calculating;
    }
    return value;
}
void FolderSizes::Invalidate(const std::wstring& path) { impl_->Invalidate(path); }
bool FolderSizes::TakeChanged() { return impl_->changed.exchange(false); }
void FolderSizes::Stop() {
    auto& s = *impl_; s.stopping = true; s.wake.notify_all();
    if (s.index_cancel) SetEvent(s.index_cancel);
    if (s.worker.joinable()) {
        CancelSynchronousIo(s.worker.native_handle());
        std::unique_lock lock(s.mutex);
        const bool finished = s.finished.wait_for(lock, std::chrono::milliseconds(100), [&] { return s.worker_finished; });
        lock.unlock();
        if (finished) s.worker.join();
        else s.worker.detach();
    }
}
#ifdef PULSE_FOLDER_SIZES_TESTING
void FolderSizes::SetScanHookForTest(std::function<void(const std::wstring&)> hook) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->worker.joinable()) impl_->scan_hook = std::move(hook);
}
unsigned FolderSizes::LiveWorkersForTest() { return live_workers.load(); }
unsigned FolderSizes::SyncRevisionForTest() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->sync_revision;
}
#endif
}
