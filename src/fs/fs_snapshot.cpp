// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// fs_snapshot.cpp
#include "fs_snapshot.h"
#include "fs_enum.h"
#include <algorithm>
#include <iterator>

namespace pulse::fs {

bool QueryDirectoryIdentity(const std::wstring& path, DirectoryIdentity& out) {
    out = {};
    if (path.empty() || IsVirtualPath(path)) return false;
    const std::wstring normalized = NormalizePath(path);
    HANDLE handle = CreateFileW(
        normalized.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != FALSE;
    CloseHandle(handle);
    if (!ok) return false;
    out.volume_serial = info.dwVolumeSerialNumber;
    out.file_index_high = info.nFileIndexHigh;
    out.file_index_low = info.nFileIndexLow;
    return out.valid();
}

bool SameDirectoryIdentity(const DirectoryIdentity& a,
                           const DirectoryIdentity& b) noexcept {
    return a.valid() && b.valid() &&
           a.volume_serial == b.volume_serial &&
           a.file_index_high == b.file_index_high &&
           a.file_index_low == b.file_index_low;
}

SnapshotStore::SnapshotStore(size_t capacity, size_t byte_capacity)
    : capacity_((std::max<size_t>)(1, capacity)), byte_capacity_(byte_capacity) {
}

size_t SnapshotStore::SnapshotBytes(const SnapshotPtr& snapshot) const {
    if (!snapshot) return 0;
    size_t bytes = sizeof(std::vector<DirEntry>) + snapshot->capacity() * sizeof(DirEntry);
    if (byte_capacity_ && bytes > byte_capacity_) return bytes;
    // Inline strings are already included in the vector's DirEntry storage.
    const auto heap_bytes = [inline_capacity = std::wstring{}.capacity()](const std::wstring& value) {
        return value.capacity() > inline_capacity ? (value.capacity() + 1) * sizeof(wchar_t) : 0;
    };
    for (const auto& entry : *snapshot) {
        bytes += heap_bytes(entry.name) + heap_bytes(entry.full_path) + heap_bytes(entry.shell_id) +
                 heap_bytes(entry.recycle_path) + heap_bytes(entry.change_type_text) +
                 heap_bytes(entry.change_old_path) + heap_bytes(entry.link_target);
        if (byte_capacity_ && bytes > byte_capacity_) break;
    }
    return bytes;
}

SnapshotStore::Entry& SnapshotStore::InsertEntry(const std::wstring& key) {
    lru_.push_front(key);
    try {
        auto& entry = map_.try_emplace(key).first->second;
        entry.lru_it = lru_.begin();
        return entry;
    } catch (...) {
        lru_.pop_front();
        throw;
    }
}

void SnapshotStore::StoreSnapshot(Entry& entry, SnapshotPtr&& snapshot, size_t bytes) {
    resident_bytes_ -= entry.bytes;
    if (byte_capacity_ && bytes > byte_capacity_) {
        entry.snapshot.reset();
        entry.bytes = 0;
    } else {
        entry.snapshot = std::move(snapshot);
        entry.bytes = bytes;
        resident_bytes_ += bytes;
    }
    entry.dirty = false;
    lru_.splice(lru_.begin(), lru_, entry.lru_it);
    EvictToBudget();
}

void SnapshotStore::Evict(EntryMap::iterator it) {
    resident_bytes_ -= it->second.bytes;
    lru_.erase(it->second.lru_it);
    map_.erase(it);
}

void SnapshotStore::EvictToBudget() {
    while (map_.size() > capacity_) Evict(map_.find(lru_.back()));

    auto next = lru_.end();
    while (byte_capacity_ && resident_bytes_ > byte_capacity_) {
        const auto oldest = std::prev(next);
        const auto it = map_.find(*oldest);
        if (it->second.bytes == 0) {
            next = oldest;
        } else {
            Evict(it);
        }
    }
}

SnapshotPtr SnapshotStore::GetOrStart(const std::wstring& path, uint64_t& out_generation,
                                      DirectoryIdentity* out_identity) {
    std::wstring key = NormalizePath(path);
    if (out_identity) *out_identity = {};
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        if (out_identity) *out_identity = it->second.identity;
        // Move to front of LRU.
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        if (!it->second.dirty && it->second.snapshot) {
            out_generation = it->second.generation;
            return it->second.snapshot;
        }
        it->second.dirty = false;
        out_generation = it->second.generation;
        return nullptr;
    }

    InsertEntry(key).dirty = false;
    EvictToBudget();
    out_generation = 0;
    return nullptr;
}

SnapshotPtr SnapshotStore::Peek(const std::wstring& path) const {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    return it->second.snapshot;
}

DirectoryIdentity SnapshotStore::Identity(const std::wstring& path) const {
    const std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    return it == map_.end() ? DirectoryIdentity{} : it->second.identity;
}

bool SnapshotStore::IsDirty(const std::wstring& path) const {
    const std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    return it != map_.end() && it->second.dirty;
}

void SnapshotStore::Update(const std::wstring& path, uint64_t generation,
                           SnapshotPtr snapshot, DirectoryIdentity identity) {
    std::wstring key = NormalizePath(path);
    // Snapshots are immutable; count large directories before taking the UI-facing lock.
    const size_t bytes = SnapshotBytes(snapshot);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it != map_.end() && generation < it->second.generation) return;
    Entry& entry = it == map_.end() ? InsertEntry(key) : it->second;
    entry.identity = identity;
    entry.generation = generation;
    StoreSnapshot(entry, std::move(snapshot), bytes);
}

uint64_t SnapshotStore::Put(const std::wstring& path, SnapshotPtr snapshot) {
    std::wstring key = NormalizePath(path);
    // Snapshots are immutable; count large directories before taking the UI-facing lock.
    const size_t bytes = SnapshotBytes(snapshot);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    Entry& entry = it == map_.end() ? InsertEntry(key) : it->second;
    StoreSnapshot(entry, std::move(snapshot), bytes);
    return entry.generation;
}

void SnapshotStore::MarkDirty(const std::wstring& path) {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) it->second.dirty = true;
}

size_t SnapshotStore::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

size_t SnapshotStore::ResidentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_bytes_;
}

} // namespace pulse::fs
