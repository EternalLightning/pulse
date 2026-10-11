// fs_net_cache.cpp — Disk snapshots for UNC folders and a timed connectivity probe.
#include "fs_net_cache.h"
#include "path_error.h"
#include <shlobj.h>
#include <chrono>
#include <fstream>
#include <string>
#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <new>
#include <unordered_map>

namespace pulse::fs {

struct NetSnapshotWriteState {
    uint64_t key = 0;
    std::atomic<uint64_t> next_sequence{1};
    std::mutex commit_mutex;
    uint64_t committed_sequence = 0;
};

#ifdef PULSE_NET_CACHE_TESTING
std::wstring NetCacheTestDirectory();
#endif

namespace {

std::wstring CacheDir() {
#ifdef PULSE_NET_CACHE_TESTING
    return NetCacheTestDirectory();
#else
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return L"";
    std::wstring dir = std::wstring(path) + L"\\Pulse";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\netcache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
#endif
}

uint64_t HashPath(const std::wstring& p) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : p) {
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring CacheFile(const std::wstring& path) {
    std::wstring dir = CacheDir();
    if (dir.empty()) return L"";
    wchar_t name[32];
    swprintf_s(name, L"%016llX.bin", HashPath(NormalizePath(path)));
    return dir + L"\\" + name;
}

uint64_t NowUnix() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

NetSnapshotWriteTicket ReserveNetSnapshotWrite(const std::wstring& path) {
    NetSnapshotWriteTicket ticket;
    if (!IsUncPath(path)) return ticket;
    static std::mutex registry_mutex;
    static std::unordered_map<uint64_t, std::weak_ptr<NetSnapshotWriteState>> writers;
    const auto key = HashPath(NormalizePath(path));
    {
        std::lock_guard lock(registry_mutex);
        if (writers.size() > 128)
            std::erase_if(writers, [](const auto& entry) { return entry.second.expired(); });
        ticket.state_ = writers[key].lock();
        if (!ticket.state_) {
            ticket.state_ = std::make_shared<NetSnapshotWriteState>();
            ticket.state_->key = key;
            writers[key] = ticket.state_;
        }
    }
    ticket.sequence_ = ticket.state_->next_sequence.fetch_add(1, std::memory_order_relaxed);
    return ticket;
}

bool SaveNetSnapshot(const std::wstring& path, const SnapshotPtr& snapshot,
                     const std::function<bool()>& cancelled, const NetSnapshotWriteTicket& requested_ticket) {
    if (!snapshot || !IsUncPath(path)) return false;
    if (cancelled && cancelled()) return false;
    const auto ticket = requested_ticket.state_ ? requested_ticket : ReserveNetSnapshotWrite(path);
    if (!ticket.state_ || ticket.state_->key != HashPath(NormalizePath(path))) return false;
    const std::wstring file = CacheFile(path);
    if (file.empty()) return false;
    static std::atomic<uint64_t> write_sequence{1};
    const std::wstring tmp = file + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(write_sequence.fetch_add(1, std::memory_order_relaxed));
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("PNCH", 4);
    uint32_t ver = 1;
    uint64_t ts = NowUnix();
    uint32_t count = static_cast<uint32_t>(snapshot->size());
    f.write(reinterpret_cast<const char*>(&ver), 4);
    f.write(reinterpret_cast<const char*>(&ts), 8);
    f.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& e : *snapshot) {
        if (cancelled && cancelled()) {
            f.close();
            DeleteFileW(tmp.c_str());
            return false;
        }
        uint8_t flags = (e.is_dir ? 1 : 0) | (e.is_reparse ? 2 : 0);
        uint64_t mtime = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        uint32_t nlen = static_cast<uint32_t>(e.name.size());
        f.write(reinterpret_cast<const char*>(&flags), 1);
        f.write(reinterpret_cast<const char*>(&e.attrs), 4);
        f.write(reinterpret_cast<const char*>(&e.size), 8);
        f.write(reinterpret_cast<const char*>(&mtime), 8);
        f.write(reinterpret_cast<const char*>(&nlen), 4);
        f.write(reinterpret_cast<const char*>(e.name.data()), nlen * sizeof(wchar_t));
    }
    f.close();
    if (!f) { DeleteFileW(tmp.c_str()); return false; }
    std::lock_guard commit_lock(ticket.state_->commit_mutex);
    if (ticket.sequence_ <= ticket.state_->committed_sequence) { DeleteFileW(tmp.c_str()); return false; }
    if (cancelled && cancelled()) { DeleteFileW(tmp.c_str()); return false; }
    if (MoveFileExW(tmp.c_str(), file.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ticket.state_->committed_sequence = ticket.sequence_;
        return true;
    }
    DeleteFileW(tmp.c_str());
    return false;
}

SnapshotPtr LoadNetSnapshot(const std::wstring& path, uint64_t* unix_sec) {
    if (!IsUncPath(path)) return nullptr;
    const std::wstring file = CacheFile(path);
    if (file.empty()) return nullptr;
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    if (!f) return nullptr;
    const std::streamoff file_bytes = f.tellg();
    constexpr std::streamoff kHeaderBytes = 20;
    constexpr std::streamoff kEntryHeaderBytes = 25;
    if (file_bytes < kHeaderBytes) return nullptr;
    f.seekg(0);
    char magic[4]{};
    f.read(magic, 4);
    if (std::string(magic, 4) != "PNCH") return nullptr;
    uint32_t ver = 0, count = 0;
    uint64_t ts = 0;
    f.read(reinterpret_cast<char*>(&ver), 4);
    f.read(reinterpret_cast<char*>(&ts), 8);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || ver != 1 || count > 500000 ||
        count > static_cast<uint64_t>((file_bytes - kHeaderBytes) / kEntryHeaderBytes))
        return nullptr;
    auto entries = std::make_shared<std::vector<DirEntry>>();
    entries->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t flags = 0;
        uint32_t attrs = 0, nlen = 0;
        uint64_t size = 0, mtime = 0;
        f.read(reinterpret_cast<char*>(&flags), 1);
        f.read(reinterpret_cast<char*>(&attrs), 4);
        f.read(reinterpret_cast<char*>(&size), 8);
        f.read(reinterpret_cast<char*>(&mtime), 8);
        f.read(reinterpret_cast<char*>(&nlen), 4);
        if (!f || nlen > 1024) return nullptr;
        DirEntry e;
        e.name.assign(nlen, L'\0');
        f.read(reinterpret_cast<char*>(e.name.data()), nlen * sizeof(wchar_t));
        if (!f) return nullptr;
        e.attrs = attrs;
        e.size = size;
        e.mtime.dwLowDateTime = static_cast<DWORD>(mtime);
        e.mtime.dwHighDateTime = static_cast<DWORD>(mtime >> 32);
        e.is_dir = (flags & 1) != 0;
        e.is_reparse = (flags & 2) != 0;
        entries->push_back(std::move(e));
    }
    if (unix_sec) *unix_sec = ts;
    return entries;
}


namespace {

struct ProbeJob {
    std::wstring unc;
    UncProbeId probe_id = 0;
    HWND hwnd = nullptr;
    UINT msg = 0;
    HANDLE done = nullptr;
    BOOL ok = FALSE;
    DWORD error_code = ERROR_SUCCESS;
    DWORD rtt_ms = 0;
    HANDLE thread = nullptr;
    uint32_t slot = 0;
    bool waiter_started = true;
    std::atomic<bool> timed_out{false};
    DWORD timeout_ms = 1500;
#ifdef PULSE_NET_CACHE_TESTING
    std::function<bool(const std::wstring&)> hook;
#endif
    ~ProbeJob() {
        if (thread) CloseHandle(thread);
        if (done) CloseHandle(done);
    }
};

constexpr uint32_t kProbeLimit = 4;
struct ProbePool {
    std::mutex mutex;
    std::array<std::shared_ptr<ProbeJob>, kProbeLimit> slots;
    struct Waiter { HWND hwnd; UINT msg; };
    std::vector<Waiter> waiters;
    void WakeLocked() {
        for (const auto& waiter : waiters)
            PostMessageW(waiter.hwnd, waiter.msg, 1, 0);
        waiters.clear();
    }
#ifdef PULSE_NET_CACHE_TESTING
    std::function<bool(const std::wstring&)> hook;
    DWORD timeout_ms = 1500;
#endif
    void ReapLocked() {
        bool released = false;
        for (auto& slot : slots)
            if (slot && !slot->waiter_started && slot->thread &&
                WaitForSingleObject(slot->thread, 0) == WAIT_OBJECT_0) { slot.reset(); released = true; }
        if (released) WakeLocked();
    }
};

ProbePool& Probes() {
    // Never destroy IO-owned jobs when an unresponsive provider outlives shutdown.
    static auto* pool = new ProbePool;
    return *pool;
}

DWORD WINAPI ProbeInner(LPVOID param) {
    auto* j = static_cast<ProbeJob*>(param);
    std::shared_ptr<ProbeJob> owner;
    {
        auto& pool = Probes();
        std::lock_guard lock(pool.mutex);
        owner = pool.slots[j->slot];
    }
    const ULONGLONG t0 = GetTickCount64();
    try {
        SetLastError(ERROR_SUCCESS);
#ifdef PULSE_NET_CACHE_TESTING
        if (j->hook) j->ok = j->hook(j->unc) ? TRUE : FALSE;
        else
#endif
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            j->ok = GetFileAttributesExW(j->unc.c_str(), GetFileExInfoStandard, &fad);
        }
        if (!j->ok) {
            j->error_code = GetLastError();
            if (!j->error_code) j->error_code = ERROR_GEN_FAILURE;
        }
    } catch (...) { j->ok = FALSE; j->error_code = ERROR_GEN_FAILURE; }
    j->rtt_ms = static_cast<DWORD>(GetTickCount64() - t0);
    SetEvent(j->done);
    // If the waiter could not start, the IO owner still releases capacity only
    // after the provider call returns, while retaining the job through exit.
    {
        auto& pool = Probes();
        std::lock_guard lock(pool.mutex);
        if (!j->waiter_started && pool.slots[j->slot] == owner) {
            pool.slots[j->slot].reset();
            pool.WakeLocked();
        }
    }
    return 0;
}

void PostProbeResult(const std::shared_ptr<ProbeJob>& job, bool timeout) {
    try {
        auto result = std::make_unique<UncProbeResult>();
        result->probe_id = job->probe_id;
        result->unc = job->unc;
        result->rtt_ms = timeout ? 0 : job->rtt_ms;
        result->error_code = timeout ? ERROR_TIMEOUT : job->error_code;
        result->final = !timeout;
        result->status = result->error_code == ERROR_SUCCESS
            ? (job->rtt_ms > 800 ? NetStatus::Slow : NetStatus::Online)
            : ClassifyPathError(result->error_code) == PathError::Offline ? NetStatus::Offline : NetStatus::Unknown;
        if (PostMessageW(job->hwnd, job->msg, 0, reinterpret_cast<LPARAM>(result.get())))
            result.release();
    } catch (...) {}
}

DWORD WINAPI ProbeWaiter(void* parameter) {
    std::unique_ptr<std::shared_ptr<ProbeJob>> argument(
        static_cast<std::shared_ptr<ProbeJob>*>(parameter));
    const auto job = *argument;
    const bool timeout = WaitForSingleObject(job->done, job->timeout_ms) != WAIT_OBJECT_0;
    job->timed_out.store(timeout, std::memory_order_release);
    PostProbeResult(job, timeout);
    // A logical deadline never releases the physical admission slot or owner.
    if (WaitForSingleObject(job->thread, INFINITE) != WAIT_OBJECT_0) {
        auto& pool = Probes();
        std::lock_guard lock(pool.mutex);
        job->waiter_started = false;
        pool.ReapLocked();
        return 0;
    }
    if (timeout) PostProbeResult(job, false);
    auto& pool = Probes();
    std::lock_guard lock(pool.mutex);
    if (pool.slots[job->slot] == job) {
        pool.slots[job->slot].reset();
        pool.WakeLocked();
    }
    return 0;
}

} // namespace

bool StartUncProbe(HWND hwnd, UINT msg, std::wstring unc, UncProbeId probe_id) {
    if (!hwnd || unc.empty() || probe_id == 0) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    auto& pool = Probes();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    uint32_t slot = 0;
    while (slot < kProbeLimit && pool.slots[slot]) ++slot;
    if (slot == kProbeLimit) {
        try {
            if (std::none_of(pool.waiters.begin(), pool.waiters.end(),
                [&](const auto& waiter) { return waiter.hwnd == hwnd && waiter.msg == msg; }))
                pool.waiters.push_back({hwnd, msg});
        } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
        SetLastError(ERROR_BUSY);
        return false;
    }
    std::shared_ptr<ProbeJob> job;
    try {
        job = std::make_shared<ProbeJob>();
#ifdef PULSE_NET_CACHE_TESTING
        job->hook = pool.hook;
        job->timeout_ms = pool.timeout_ms;
#endif
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    job->slot = slot;
    job->unc = std::move(unc);
    job->probe_id = probe_id;
    job->hwnd = hwnd;
    job->msg = msg;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->done) return false;
    pool.slots[slot] = job;
    job->thread = CreateThread(nullptr, 0, ProbeInner, job.get(), 0, nullptr);
    if (!job->thread) { pool.slots[slot].reset(); return false; }
    auto* argument = new (std::nothrow) std::shared_ptr<ProbeJob>(job);
    HANDLE waiter = argument ? CreateThread(nullptr, 0, ProbeWaiter, argument, 0, nullptr) : nullptr;
    if (!waiter) {
        const DWORD error = argument ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        delete argument;
        job->waiter_started = false;
        pool.ReapLocked();
        SetLastError(error);
        return false;
    }
    CloseHandle(waiter);
    return true;
}

#ifdef PULSE_NET_CACHE_TESTING
UncProbeResourceUsage UncProbeResourcesForTest() {
    auto& pool = Probes();
    std::lock_guard lock(pool.mutex);
    pool.ReapLocked();
    UncProbeResourceUsage usage{};
    usage.limit = kProbeLimit;
    for (const auto& slot : pool.slots) if (slot) {
        ++usage.live;
        if (slot->timed_out.load(std::memory_order_acquire)) ++usage.retired;
    }
    return usage;
}

void SetUncProbeHookForTest(std::function<bool(const std::wstring&)> hook, DWORD timeout_ms) {
    auto& pool = Probes();
    std::lock_guard lock(pool.mutex);
    pool.hook = std::move(hook);
    pool.timeout_ms = timeout_ms ? timeout_ms : 1;
}
#endif

} // namespace pulse::fs
