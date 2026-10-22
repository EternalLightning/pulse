#pragma once

#include "change_tracking.h"
#include "index_query.h"
#include "index_types.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace pulse::index {

#ifdef PULSE_NETWORK_INDEX_TESTING
namespace testing {
struct NetworkIndexHooks {
    decltype(&FindFirstFileExW) find_first = nullptr;
    decltype(&FindNextFileW) find_next = nullptr;
    decltype(&FindClose) find_close = nullptr;
    DWORD (WINAPI* universal_name)(LPCWSTR, DWORD, LPVOID, LPDWORD) = nullptr;
    std::wstring shard_base;
    std::wstring config_path;
};
void SetNetworkIndexHooks(const NetworkIndexHooks* hooks);
struct NetworkIndexAccess;
bool MatchNetworkFilenameForTest(const Hit& hit, const CompiledQuery& query);
}
#endif

struct NetworkRootInfo {
    std::wstring path;
    bool online = false;
    bool building = false;
    bool watching = false;
    uint64_t indexed_items = 0;
    uint32_t progress = 0;
    std::wstring state;
    std::wstring error;
};

std::wstring NormalizeNetworkRoot(std::wstring path);
std::wstring NetworkConfigPath();
enum class NetworkConfigLoadStatus { MissingFile, Empty, Loaded, MissingRoots, Invalid, IoError };

bool LoadNetworkRootsFile(const std::wstring& path, std::vector<std::wstring>& roots,
                          std::wstring* error = nullptr, NetworkConfigLoadStatus* status = nullptr);
bool SaveNetworkRootsFile(const std::wstring& path, const std::vector<std::wstring>& roots,
                          std::wstring* error = nullptr);
bool LoadNetworkRoots(std::vector<std::wstring>& roots, std::wstring* error = nullptr);
bool SaveNetworkRoots(const std::vector<std::wstring>& roots, std::wstring* error = nullptr);

// Per-user SMB index. It intentionally runs in Pulse.Index.exe's
// --network-agent mode so Windows can use the interactive user's existing SMB
// session without the SYSTEM service storing credentials.
class NetworkIndex {
public:
    NetworkIndex() = default;
    ~NetworkIndex() { Stop(); }
    NetworkIndex(const NetworkIndex&) = delete;
    NetworkIndex& operator=(const NetworkIndex&) = delete;

    void Start(HWND notify, UINT status_msg, UINT search_msg);
    void Stop();
    void SearchAsync(const Query& query, uint32_t id);
    bool TakeResult(uint32_t id, SearchResult& result);

    std::vector<NetworkRootInfo> Roots() const;
    std::wstring ConfigurationError() const;
    bool AddRoot(const std::wstring& path, std::wstring* error = nullptr);
    bool RemoveRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Rebuild(const std::wstring& path = {});
    ChangeTracker& Changes() { return changes_; }
    void SetChangeLease(const std::wstring& owner, bool enabled);
    ChangeState ChangeCoverage(const std::wstring& path) const;

private:
#ifdef PULSE_NETWORK_INDEX_TESTING
    friend struct testing::NetworkIndexAccess;
#endif
    ChangeTracker changes_;
    std::mutex change_seed_mutex_;
    std::unordered_set<std::wstring> change_seed_owners_;
    void SeedChanges(const std::wstring& owner);
    void SeedPendingChanges();
    void ObserveChanges(const std::wstring& root, const BYTE* data, DWORD bytes);
    struct Shard;
    struct RootState {
        NetworkRootInfo info;
        std::shared_ptr<Shard> shard;
    };

    bool LoadConfiguration(std::vector<std::wstring>& configured);
    std::wstring config_error_;
    void CrawlLoop();
    void WatchLoop();
    void SearchLoop();
    void BuildRoot(const std::wstring& path, uint64_t generation);
    void NotifyStatus() const;
    bool RootStillCurrent(const std::wstring& path, uint64_t generation) const;

    HWND notify_ = nullptr;
    UINT status_msg_ = 0;
    UINT search_msg_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> latest_search_id_{0};
    mutable std::mutex mu_;
    std::condition_variable crawl_cv_;
    std::condition_variable search_cv_;
    std::vector<RootState> roots_;
    std::unordered_set<std::wstring> dirty_roots_;
    uint64_t generation_ = 1;
    Query pending_query_;
    uint32_t pending_id_ = 0;
    bool have_pending_search_ = false;
    uint32_t result_id_ = 0;
    SearchResult result_;
    std::thread crawl_thread_;
    std::thread watch_thread_;
    std::thread search_thread_;
    HANDLE watch_wake_event_ = nullptr;
};

} // namespace pulse::index
