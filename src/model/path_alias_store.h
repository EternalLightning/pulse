// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "path_aliases.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace pulse::model {
enum class PathAliasStoreState { Loading, Ready, Saving, Unreadable, SaveFailed };
struct PathAliasSnapshot {
    PathAliases aliases;
    PathAliasStoreState state = PathAliasStoreState::Loading;
    uint64_t revision = 0;
};
class PathAliasStore {
public:
    ~PathAliasStore();
    void Start(std::wstring path, std::function<void()> changed);
    PathAliasSnapshot Snapshot() const;
    bool Commit(PathAliases aliases, uint64_t revision);
private:
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::wstring path_;
    std::function<void()> changed_;
    PathAliasSnapshot current_;
    PathAliases pending_;
    bool stopping_ = false, pending_save_ = false;
};
} // namespace pulse::model
