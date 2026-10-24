// index_shard.h — stable per-source shard paths and atomic generation manifest.
#pragma once
#include <cstdint>
#include <string>

namespace pulse::index {

// V12 stores bucket-local delta varints instead of raw filename posting IDs.
inline constexpr uint32_t kIndexSnapshotVersion = 12;

struct ShardPaths {
    std::wstring directory;
    std::wstring manifest;
    std::wstring base_a;
    std::wstring base_b;
    std::wstring wal_a;
    std::wstring wal_b;
};

uint64_t ShardIdHash(const std::wstring& source_id);
ShardPaths MakeShardPaths(const std::wstring& root, const std::wstring& source_id);

struct ShardManifest {
    uint32_t version = 9;
    uint64_t generation = 0;
    uint64_t active_built = 0;
    uint64_t active_wal_bytes = 0;
    uint8_t active_slot = 0;
    uint8_t previous_slot = 1;
    uint16_t reserved = 0;
    uint64_t active_bytes = 0;
    uint64_t active_crc64 = 0;
    std::wstring source_id;
};

enum class ShardReadStatus { Missing, Invalid, IoError, Valid, Unsupported };

// Missing manifests retain the legacy true result; status distinguishes absence.
bool LoadShardManifest(const std::wstring& path, ShardManifest& out, std::wstring* error = nullptr,
                       ShardReadStatus* status = nullptr);
bool SaveShardManifest(const std::wstring& path, const ShardManifest& value,
                       std::wstring* error = nullptr);

// V9 storage operations. A base file is treated as opaque here; the index
// engine validates its PIDX header after publication before adopting it.
uint64_t FileCrc64(const std::wstring& path, ShardReadStatus* status = nullptr);
bool ValidateShardBase(const std::wstring& path, uint64_t expected_bytes = 0,
                       uint64_t expected_crc64 = 0, ShardReadStatus* status = nullptr);
bool ResolveActiveShard(const ShardPaths& paths, ShardManifest& manifest,
                        std::wstring& active_path, std::wstring* error = nullptr,
                        ShardReadStatus* status = nullptr);
bool PublishShardBase(const ShardPaths& paths, const std::wstring& temp_path,
                      uint64_t built_unix, uint64_t wal_bytes,
                      ShardManifest& published, std::wstring* error = nullptr,
                      const std::wstring& source_id = {});

} // namespace pulse::index
