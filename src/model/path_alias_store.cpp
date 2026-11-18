// SPDX-License-Identifier: Apache-2.0
#include "path_alias_store.h"
#include "../common/utf8_file.h"

namespace pulse::model {
PathAliasStore::~PathAliasStore() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void PathAliasStore::Start(std::wstring path, std::function<void()> changed) {
    std::lock_guard lock(mutex_);
    if (worker_.joinable()) return;
    path_ = std::move(path);
    changed_ = std::move(changed);
    worker_ = std::thread([this] { Run(); });
}
PathAliasSnapshot PathAliasStore::Snapshot() const {
    std::lock_guard lock(mutex_);
    return current_;
}
bool PathAliasStore::Commit(PathAliases aliases, uint64_t revision) {
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable() || current_.revision != revision ||
            (current_.state != PathAliasStoreState::Ready && current_.state != PathAliasStoreState::SaveFailed)) return false;
        pending_ = std::move(aliases);
        pending_save_ = true;
        current_.state = PathAliasStoreState::Saving;
    }
    wake_.notify_one();
    changed_();
    return true;
}
void PathAliasStore::Run() {
    std::wstring text;
    PathAliases loaded;
    const bool read = ReadUtf8File(path_, text);
    const DWORD read_error = read ? ERROR_SUCCESS : GetLastError();
    const bool missing = !read && (read_error == ERROR_FILE_NOT_FOUND || read_error == ERROR_PATH_NOT_FOUND);
    const bool valid = read && loaded.LoadJson(text);
    {
        std::lock_guard lock(mutex_);
        if (valid) current_.aliases = std::move(loaded);
        current_.state = valid || missing ? PathAliasStoreState::Ready : PathAliasStoreState::Unreadable;
        ++current_.revision;
    }
    changed_();
    for (;;) {
        PathAliases next;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || pending_save_; });
            if (!pending_save_) return;
            next = std::move(pending_);
            pending_save_ = false;
        }
        const auto parent = path_.find_last_of(L"\\/");
        if (parent != std::wstring::npos) CreateDirectoryW(path_.substr(0, parent).c_str(), nullptr);
        const bool saved = WriteUtf8FileAtomic(path_, next.ToJson());
        {
            std::lock_guard lock(mutex_);
            if (saved) { current_.aliases = std::move(next); ++current_.revision; }
            current_.state = saved ? PathAliasStoreState::Ready : PathAliasStoreState::SaveFailed;
        }
        changed_();
    }
}
} // namespace pulse::model
