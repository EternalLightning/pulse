// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "index_engine.h"
#include "filename_pinyin.h"
#include <array>
#include <filesystem>
#include <fstream>
namespace pulse::index {
namespace {
using SidecarIdentity = std::array<uint64_t, 10>;
constexpr uint64_t kPinyinSidecarVersion = 2;
void HashIds(uint64_t& hash, const std::vector<int32_t>& ids) {
    hash = (hash ^ ids.size()) * 1099511628211ull;
    for (int32_t id : ids) hash = (hash ^ static_cast<uint32_t>(id)) * 1099511628211ull;
}
SidecarIdentity Identity(HANDLE file, const DiskHeader& header) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return {};
    return {0x31595045534c5550ull, (kPinyinSidecarVersion << 32) | kPinyinDataVersion, header.built_unix,
        header.node_count, header.pool_chars, info.dwVolumeSerialNumber,
        (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        sizeof(wchar_t)};
}
std::wstring SidecarPath(HANDLE file) {
    std::wstring path(32768, L'\0');
    const DWORD count = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED);
    if (!count || count >= path.size()) return {};
    path.resize(count);
    return path + L".pinyin-v2";
}
bool ReadSidecar(const std::wstring& path, const SidecarIdentity& identity,
                 std::vector<int32_t>& chinese, std::vector<std::vector<int32_t>>& pairs) {
    if (path.empty() || !identity[0]) return false;
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    SidecarIdentity stored{};
    if (!file.read(reinterpret_cast<char*>(stored.data()), sizeof(stored)) || stored != identity) return false;
    uint64_t hash = 1469598103934665603ull;
    auto read_ids = [&](std::vector<int32_t>& ids) {
        uint32_t count = 0;
        if (!file.read(reinterpret_cast<char*>(&count), sizeof(count)) || count > identity[3]) return false;
        ids.resize(count);
        if (!file.read(reinterpret_cast<char*>(ids.data()), static_cast<std::streamsize>(ids.size() * sizeof(int32_t)))) return false;
        HashIds(hash, ids);
        int32_t previous = -1;
        for (int32_t id : ids) {
            if (id <= previous || static_cast<uint64_t>(id) >= identity[3]) return false;
            previous = id;
        }
        return true;
    };
    if (!read_ids(chinese)) return false;
    pairs.assign(676, {});
    for (auto& ids : pairs) if (!read_ids(ids) || ids.size() > chinese.size()) return false;
    uint64_t stored_hash = 0;
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&stored_hash), sizeof(stored_hash))) &&
        hash == stored_hash && file.peek() == std::char_traits<char>::eof();
}
void WriteSidecar(const std::wstring& path, const SidecarIdentity& identity,
                  const std::vector<int32_t>& chinese, const std::vector<std::vector<int32_t>>& pairs) {
    if (path.empty() || !identity[0]) return;
    const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.write(reinterpret_cast<const char*>(identity.data()), sizeof(identity));
    uint64_t hash = 1469598103934665603ull;
    auto write_ids = [&](const std::vector<int32_t>& ids) {
        HashIds(hash, ids);
        const uint32_t count = static_cast<uint32_t>(ids.size());
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        file.write(reinterpret_cast<const char*>(ids.data()), static_cast<std::streamsize>(ids.size() * sizeof(int32_t)));
    };
    write_ids(chinese);
    for (const auto& ids : pairs) write_ids(ids);
    file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temporary.c_str());
}
}
void Engine::RequestPinyinBuildLocked() {
    pinyin_ready_ = false;
    {
        std::lock_guard<std::mutex> guard(pinyin_build_mutex_);
        ++pinyin_request_;
        if (!pinyin_thread_.joinable()) {
            pinyin_stopping_ = false;
            pinyin_thread_ = std::thread(&Engine::PinyinWorker, this);
        }
    }
    pinyin_build_cv_.notify_one();
}
void Engine::StopPinyinWorker() {
    {
        std::lock_guard<std::mutex> guard(pinyin_build_mutex_);
        pinyin_stopping_ = true;
    }
    pinyin_build_cv_.notify_one();
    if (pinyin_thread_.joinable()) pinyin_thread_.join();
    pinyin_ready_ = false;
}
void Engine::PinyinWorker() {
    uint64_t processed = 0;
    for (;;) {
        uint64_t generation;
        {
            std::unique_lock<std::mutex> guard(pinyin_build_mutex_);
            pinyin_build_cv_.wait(guard, [&] { return pinyin_stopping_.load() || pinyin_request_.load() != processed; });
            if (pinyin_stopping_) return;
            generation = pinyin_request_.load();
        }
        processed = generation;
        try {
            std::vector<int32_t> chinese;
            std::vector<std::vector<int32_t>> pairs(676);
            const MappedFile* snapshot;
            {
                // Queries may run concurrently. Snapshot replacement waits until
                // this reader finishes, so no IDs can outlive their mapped names.
                std::shared_lock<std::shared_mutex> lock(mutex_);
                if (!map_ || generation != pinyin_request_.load()) continue;
                snapshot = map_.get();
                const auto identity = Identity(snapshot->file, *snapshot->hdr);
                const auto path = SidecarPath(snapshot->file);
                if (!ReadSidecar(path, identity, chinese, pairs)) {
                    chinese.clear();
                    pairs.assign(676, {});
                    for (int32_t id = 0; id < BaseCount(); ++id) {
                        if ((id & 0x3ff) == 0 && pinyin_stopping_) return;
                        const auto& node = snapshot->nodes[id];
                        const std::wstring_view name(snapshot->pool + node.off, node.len);
                        if (!HasPinyinCharacters(name)) continue;
                        chinese.push_back(id);
                        const auto signature = PinyinCandidatePairs(name);
                        for (size_t word = 0; word < signature.size(); ++word) {
                            uint64_t mask = signature[word];
                            while (mask) {
                                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                                pairs[word * 64 + bit].push_back(id);
                                mask &= mask - 1;
                            }
                        }
                    }
                    WriteSidecar(path, identity, chinese, pairs);
                }
            }
            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                if (pinyin_stopping_ || generation != pinyin_request_.load() || map_.get() != snapshot) continue;
                pinyin_chinese_ids_ = std::move(chinese);
                pinyin_pair_ids_ = std::move(pairs);
                pinyin_snapshot_ = snapshot;
                pinyin_version_ = kPinyinDataVersion;
                pinyin_ready_ = true;
                InvalidateFilterLocked();
            }
            PingNotify(true);
        } catch (...) {
            // Auxiliary failure must not stop filename indexing or literal search.
            pinyin_ready_ = false;
        }
    }
}
}
