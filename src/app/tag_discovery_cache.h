#pragma once
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

namespace pulse::app {

class TagDiscoveryCache {
public:
    explicit TagDiscoveryCache(size_t capacity = 4096, uint64_t ttl_ms = 30000)
        : capacity_(capacity ? capacity : 1), ttl_ms_(ttl_ms) {}

    bool Contains(const std::wstring& key, uint64_t version, uint64_t now) {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return false;
        if (found->second.version != version || now - found->second.checked_at >= ttl_ms_) {
            order_.erase(found->second.position);
            entries_.erase(found);
            return false;
        }
        order_.splice(order_.end(), order_, found->second.position);
        return true;
    }

    void Record(const std::wstring& key, uint64_t version, uint64_t now) {
        Erase(key);
        while (entries_.size() >= capacity_) {
            entries_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_back(key);
        entries_.emplace(key, Entry{version, now, std::prev(order_.end())});
    }

    void Erase(const std::wstring& key) {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return;
        order_.erase(found->second.position);
        entries_.erase(found);
    }

    void Clear() { entries_.clear(); order_.clear(); }
    size_t Size() const noexcept { return entries_.size(); }

private:
    struct Entry {
        uint64_t version;
        uint64_t checked_at;
        std::list<std::wstring>::iterator position;
    };
    size_t capacity_;
    uint64_t ttl_ms_;
    std::list<std::wstring> order_;
    std::unordered_map<std::wstring, Entry> entries_;
};

} // namespace pulse::app
