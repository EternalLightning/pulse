#pragma once
#include "network_recent.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse::app {
// Shared by the process; local disk reads and atomic writes stay off the UI thread.
class NetworkRecentStore {
public:
    explicit NetworkRecentStore(bool persist, std::wstring file = {});
    ~NetworkRecentStore();
    NetworkRecentStore(const NetworkRecentStore&) = delete;
    NetworkRecentStore& operator=(const NetworkRecentStore&) = delete;
    void Touch(std::wstring path, uint64_t now);
    NetworkRecentList Snapshot(uint64_t* revision = nullptr) const;
    void Flush();
private:
    void Run();
    bool persist_;
    std::wstring file_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<NetworkRecentItem> pending_;
    NetworkRecentList list_;
    uint64_t revision_ = 0, submitted_ = 0, completed_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};
}
