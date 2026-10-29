#include "network_recent_store.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"

namespace pulse::app {
NetworkRecentStore::NetworkRecentStore(bool persist, std::wstring file)
    : persist_(persist), file_(std::move(file)), worker_([this] { Run(); }) {}
NetworkRecentStore::~NetworkRecentStore() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}
void NetworkRecentStore::Touch(std::wstring path, uint64_t now) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        pending_.push_back({std::move(path), now});
        ++submitted_;
    }
    wake_.notify_all();
}
NetworkRecentList NetworkRecentStore::Snapshot(uint64_t* revision) const {
    std::lock_guard lock(mutex_);
    if (revision) *revision = revision_;
    return list_;
}
void NetworkRecentStore::Flush() {
    std::unique_lock lock(mutex_);
    const auto target = submitted_;
    wake_.wait(lock, [&] { return revision_ != 0 && completed_ >= target; });
}
void NetworkRecentStore::Run() {
    NetworkRecentList current;
    if (persist_) {
        if (file_.empty()) {
            const auto root = storage::DefaultRoot();
            if (!root.empty()) {
                CreateDirectoryW(root.c_str(), nullptr);
                file_ = root + L"\\network_recent.json";
            }
        }
        std::wstring text;
        if (!file_.empty() && ReadUtf8File(file_, text)) current.FromJson(text);
    }
    {
        std::lock_guard lock(mutex_);
        list_ = current;
        ++revision_;
    }
    wake_.notify_all();
    for (;;) {
        std::vector<NetworkRecentItem> visits;
        uint64_t target = 0;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty() && stopping_) break;
            visits.swap(pending_);
            target = submitted_;
        }
        bool changed = false;
        for (const auto& visit : visits) changed |= current.Touch(visit.path, visit.last_visit);
        if (changed && persist_ && !file_.empty()) WriteUtf8FileAtomic(file_, current.ToJson());
        {
            std::lock_guard lock(mutex_);
            if (changed) { list_ = current; ++revision_; }
            completed_ = target;
        }
        wake_.notify_all();
    }
}
}
