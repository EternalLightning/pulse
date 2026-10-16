#include "index_shard.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <vector>

namespace pulse::index {
namespace {
void SetError(std::wstring* error, const wchar_t* text) { if (error) *error = text; }
void SetStatus(ShardReadStatus* status, ShardReadStatus value) { if (status) *status = value; }
ShardReadStatus OpenFailureStatus() {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? ShardReadStatus::Missing : ShardReadStatus::IoError;
}
bool BlocksRecovery(ShardReadStatus status) {
    return status == ShardReadStatus::IoError || status == ShardReadStatus::Unsupported;
}
bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
std::wstring Hex(uint64_t value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%016I64X", value);
    return text;
}
std::wstring ManifestJson(const ShardManifest& v) {
    std::wstring id;
    pulse::json::Escape(v.source_id, id);
    return L"{\n  \"version\":" + std::to_wstring(v.version) +
           L",\n  \"generation\":" + std::to_wstring(v.generation) +
           L",\n  \"active_built\":" + std::to_wstring(v.active_built) +
           L",\n  \"active_wal_bytes\":" + std::to_wstring(v.active_wal_bytes) +
           L",\n  \"active_slot\":" + std::to_wstring(v.active_slot) +
           L",\n  \"previous_slot\":" + std::to_wstring(v.previous_slot) +
           L",\n  \"active_bytes\":" + std::to_wstring(v.active_bytes) +
           L",\n  \"active_crc64\":" + std::to_wstring(v.active_crc64) +
           L",\n  \"source_id\":\"" + id + L"\"\n}\n";
}

bool ReadU64(const std::wstring& text, const wchar_t* key, uint64_t& value) {
    size_t pos = pulse::json::ValuePosition(text, key);
    if (pos == std::wstring::npos || pos >= text.size() || text[pos] < L'0' || text[pos] > L'9')
        return false;
    value = 0;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
        const uint64_t digit = static_cast<uint64_t>(text[pos] - L'0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
        ++pos;
    }
    while (pos < text.size() && iswspace(text[pos])) ++pos;
    return pos < text.size() && (text[pos] == L',' || text[pos] == L'}');
}

bool ReadFilePrefix(const std::wstring& path, char* magic, uint32_t& version,
                    uint64_t& built, ShardReadStatus* status = nullptr) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(status, OpenFailureStatus()); return false; }
    BYTE header[24]{};
    DWORD got = 0;
    const bool read = ReadFile(h, header, sizeof(header), &got, nullptr) != FALSE;
    CloseHandle(h);
    if (!read) { SetStatus(status, ShardReadStatus::IoError); return false; }
    if (got < sizeof(header)) { SetStatus(status, ShardReadStatus::Invalid); return false; }
    memcpy(magic, header, 4);
    memcpy(&version, header + 4, sizeof(version));
    memcpy(&built, header + 16, sizeof(built));
    SetStatus(status, ShardReadStatus::Valid);
    return true;
}

std::wstring SlotPath(const ShardPaths& paths, uint8_t slot) {
    return slot == 0 ? paths.base_a : paths.base_b;
}

bool WriteThroughFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const size_t bytes = text.size() * sizeof(wchar_t);
    DWORD written = 0;
    const bool ok = bytes <= 0xffffffffu &&
        WriteFile(h, text.data(), static_cast<DWORD>(bytes), &written, nullptr) &&
        written == bytes && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    return ok;
}
}

uint64_t ShardIdHash(const std::wstring& source_id) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : source_id) {
        hash ^= static_cast<uint16_t>(towupper(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

ShardPaths MakeShardPaths(const std::wstring& root, const std::wstring& source_id) {
    ShardPaths paths;
    if (root.empty() || source_id.empty()) return paths;
    paths.directory = root + L"\\" + Hex(ShardIdHash(source_id));
    EnsureDirectory(root);
    EnsureDirectory(paths.directory);
    paths.manifest = paths.directory + L"\\manifest.json";
    paths.base_a = paths.directory + L"\\base-a.bin";
    paths.base_b = paths.directory + L"\\base-b.bin";
    paths.wal_a = paths.directory + L"\\wal-a.log";
    paths.wal_b = paths.directory + L"\\wal-b.log";
    return paths;
}

bool LoadShardManifest(const std::wstring& path, ShardManifest& out, std::wstring* error,
                       ShardReadStatus* status) {
    out = {};
    SetStatus(status, ShardReadStatus::Invalid);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const auto failure = OpenFailureStatus();
        SetStatus(status, failure);
        if (failure == ShardReadStatus::Missing) return true;
        SetError(error, L"无法读取分片清单");
        return false;
    }
    LARGE_INTEGER size{};
    std::wstring json;
    DWORD bytes = 0;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        SetStatus(status, ShardReadStatus::IoError);
        SetError(error, L"无法读取分片清单大小");
        return false;
    }
    if (size.QuadPart > 0 &&
        size.QuadPart <= static_cast<LONGLONG>(64 * 1024) &&
        (size.QuadPart % sizeof(wchar_t)) == 0) {
        json.resize(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)));
        if (!ReadFile(h, json.data(), static_cast<DWORD>(size.QuadPart), &bytes, nullptr)) {
            CloseHandle(h);
            SetStatus(status, ShardReadStatus::IoError);
            SetError(error, L"无法读取分片清单内容");
            return false;
        }
        if (bytes != static_cast<DWORD>(size.QuadPart)) json.clear();
    }
    CloseHandle(h);
    const size_t first = json.find_first_not_of(L" \r\n\t");
    const size_t last = json.find_last_not_of(L" \r\n\t");
    uint64_t version = 0, active_slot = 0, previous_slot = 0;
    if (first == std::wstring::npos || json[first] != L'{' || json[last] != L'}' ||
        !ReadU64(json, L"version", version) || !version) {
        SetError(error, L"分片清单损坏"); return false;
    }
    if (version > 9) {
        SetStatus(status, ShardReadStatus::Unsupported);
        SetError(error, L"分片清单版本不受支持"); return false;
    }
    out.version = static_cast<uint32_t>(version);
    if (!ReadU64(json, L"generation", out.generation) ||
        !ReadU64(json, L"active_built", out.active_built) ||
        !ReadU64(json, L"active_wal_bytes", out.active_wal_bytes) ||
        !ReadU64(json, L"active_slot", active_slot) ||
        !ReadU64(json, L"previous_slot", previous_slot) || active_slot > 1 || previous_slot > 1 ||
        !ReadU64(json, L"active_bytes", out.active_bytes) ||
        !ReadU64(json, L"active_crc64", out.active_crc64)) {
        out = {}; SetError(error, L"分片清单字段无效"); return false;
    }
    out.active_slot = static_cast<uint8_t>(active_slot);
    out.previous_slot = static_cast<uint8_t>(previous_slot);
    out.source_id = pulse::json::ExtractString(json, L"source_id", L"");
    if (out.source_id.empty()) { out = {}; SetError(error, L"分片清单来源无效"); return false; }
    SetStatus(status, ShardReadStatus::Valid);
    return true;
}

bool SaveShardManifest(const std::wstring& path, const ShardManifest& value, std::wstring* error) {
    if (path.empty() || value.source_id.empty()) { SetError(error, L"分片清单参数无效"); return false; }
    const std::wstring temp = path + L".tmp";
    if (!WriteThroughFile(temp, ManifestJson(value))) {
        SetError(error, L"无法创建分片清单临时文件"); return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); SetError(error, L"无法发布分片清单"); return false;
    }
    return true;
}

uint64_t FileCrc64(const std::wstring& path, ShardReadStatus* status) {
    SetStatus(status, ShardReadStatus::IoError);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(status, OpenFailureStatus()); return 0; }
    uint64_t hash = 14695981039346656037ull;
    std::vector<BYTE> buffer(1u << 20);
    DWORD got = 0;
    bool complete = false;
    for (;;) {
        if (!ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) break;
        if (!got) { complete = true; break; }
        for (DWORD i = 0; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ull;
        }
    }
    CloseHandle(h);
    if (complete) SetStatus(status, ShardReadStatus::Valid);
    return complete ? hash : 0;
}

bool ValidateShardBase(const std::wstring& path, uint64_t expected_bytes,
                       uint64_t expected_crc64, ShardReadStatus* status) {
    SetStatus(status, ShardReadStatus::Invalid);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) { SetStatus(status, OpenFailureStatus()); return false; }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SetStatus(status, OpenFailureStatus()); return false; }
    LARGE_INTEGER size{};
    char magic[4]{};
    uint32_t version = 0;
    uint64_t built = 0;
    const bool size_ok = GetFileSizeEx(h, &size) != FALSE;
    CloseHandle(h);
    if (!size_ok) { SetStatus(status, ShardReadStatus::IoError); return false; }
    if (size.QuadPart < 128) return false;
    if (!ReadFilePrefix(path, magic, version, built, status)) return false;
    SetStatus(status, ShardReadStatus::Invalid);
    if (memcmp(magic, "PIDX", 4) != 0 || version < 7) return false;
    if (version > kIndexSnapshotVersion) { SetStatus(status, ShardReadStatus::Unsupported); return false; }
    if (expected_bytes && static_cast<uint64_t>(size.QuadPart) != expected_bytes) return false;
    if (expected_crc64) {
        ShardReadStatus hash_status;
        const uint64_t hash = FileCrc64(path, &hash_status);
        if (hash_status != ShardReadStatus::Valid) { SetStatus(status, hash_status); return false; }
        if (hash != expected_crc64) return false;
    }
    if (!built && version < 9) return false;
    SetStatus(status, ShardReadStatus::Valid);
    return true;
}

bool ResolveActiveShard(const ShardPaths& paths, ShardManifest& manifest,
                        std::wstring& active_path, std::wstring* error, ShardReadStatus* status) {
    manifest = {};
    active_path.clear();
    SetStatus(status, ShardReadStatus::Invalid);
    ShardManifest loaded;
    ShardReadStatus manifest_status;
    const bool manifest_ok = LoadShardManifest(paths.manifest, loaded, error, &manifest_status);
    if (BlocksRecovery(manifest_status)) { SetStatus(status, manifest_status); return false; }
    if (manifest_ok && !loaded.source_id.empty()) {
        const std::wstring candidate = SlotPath(paths, loaded.active_slot);
        ShardReadStatus active_status;
        if (ValidateShardBase(candidate, loaded.active_bytes, loaded.active_crc64, &active_status)) {
            manifest = loaded;
            active_path = candidate;
            SetStatus(status, ShardReadStatus::Valid);
            return true;
        }
        if (BlocksRecovery(active_status)) { SetStatus(status, active_status); return false; }
        if (loaded.previous_slot == loaded.active_slot) {
            SetError(error, L"分片清单未指定独立回退槽");
            return false;
        }
        const uint8_t fallback_slot = loaded.previous_slot;
        const std::wstring fallback = SlotPath(paths, fallback_slot);
        ShardReadStatus fallback_status;
        if (ValidateShardBase(fallback, 0, 0, &fallback_status)) {
            char magic[4]{};
            uint32_t version = 0;
            uint64_t fallback_built = 0;
            if (!ReadFilePrefix(fallback, magic, version, fallback_built)) {
                SetStatus(status, ShardReadStatus::IoError); return false;
            }
            loaded.active_slot = fallback_slot;
            loaded.active_built = fallback_built;
            HANDLE h = CreateFileW(fallback.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER size{};
            loaded.active_bytes = h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size)
                ? static_cast<uint64_t>(size.QuadPart) : 0;
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            loaded.active_crc64 = FileCrc64(fallback);
            if (!loaded.active_bytes || !loaded.active_crc64) {
                SetStatus(status, ShardReadStatus::IoError);
                SetError(error, L"无法完整读取回退分片，未更新分片清单");
                return false;
            }
            loaded.previous_slot = static_cast<uint8_t>(1 - loaded.active_slot);
            loaded.generation = loaded.generation ? loaded.generation - 1 : 0;
            if (!SaveShardManifest(paths.manifest, loaded, error)) {
                SetStatus(status, ShardReadStatus::IoError); return false;
            }
            manifest = loaded;
            active_path = fallback;
            SetStatus(status, ShardReadStatus::Valid);
            return true;
        }
        if (BlocksRecovery(fallback_status)) { SetStatus(status, fallback_status); return false; }
        SetStatus(status, active_status == ShardReadStatus::Missing && fallback_status == ShardReadStatus::Missing
            ? ShardReadStatus::Missing : ShardReadStatus::Invalid);
        // A manifest-rejected slot must not be retried without its checksum.
        SetError(error, L"清单指定的分片及回退分片均不可用");
        return false;
    }

    uint64_t best_built = 0;
    uint8_t best_slot = 0;
    bool found = false;
    bool all_missing = true;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        const std::wstring candidate = SlotPath(paths, slot);
        ShardReadStatus candidate_status;
        if (!ValidateShardBase(candidate, 0, 0, &candidate_status)) {
            if (BlocksRecovery(candidate_status)) { SetStatus(status, candidate_status); return false; }
            all_missing &= candidate_status == ShardReadStatus::Missing;
            continue;
        }
        all_missing = false;
        FileCrc64(candidate, &candidate_status);
        if (candidate_status != ShardReadStatus::Valid) {
            SetStatus(status, ShardReadStatus::IoError); return false;
        }
        char magic[4]{};
        uint32_t version = 0;
        uint64_t built = 0;
        if (!ReadFilePrefix(candidate, magic, version, built)) {
            SetStatus(status, ShardReadStatus::IoError); return false;
        }
        if (!found || built >= best_built) {
            found = true;
            best_built = built;
            best_slot = slot;
        }
    }
    if (!found) {
        SetStatus(status, all_missing ? ShardReadStatus::Missing : ShardReadStatus::Invalid);
        if (error && error->empty()) *error = L"没有可用的 V9 分片";
        return false;
    }
    manifest = {};
    manifest.version = 9;
    manifest.generation = 1;
    manifest.active_built = best_built;
    manifest.active_slot = best_slot;
    manifest.previous_slot = static_cast<uint8_t>(1 - best_slot);
    manifest.source_id = L"unknown";
    active_path = SlotPath(paths, best_slot);
    SetStatus(status, ShardReadStatus::Valid);
    return true;
}

bool PublishShardBase(const ShardPaths& paths, const std::wstring& temp_path,
                      uint64_t built_unix, uint64_t wal_bytes,
                      ShardManifest& published, std::wstring* error,
                      const std::wstring& source_id) {
    if (!ValidateShardBase(temp_path)) {
        SetError(error, L"V9 base 校验失败");
        return false;
    }
    ShardManifest current;
    std::wstring active;
    ShardReadStatus status;
    if (!ResolveActiveShard(paths, current, active, nullptr, &status)) {
        if (BlocksRecovery(status)) {
            SetError(error, L"现有分片暂不可读或版本不受支持，未发布新分片");
            return false;
        }
        ShardManifest existing;
        ShardReadStatus manifest_status;
        LoadShardManifest(paths.manifest, existing, nullptr, &manifest_status);
        if (BlocksRecovery(manifest_status) || (manifest_status == ShardReadStatus::Valid &&
            existing.previous_slot == existing.active_slot)) {
            SetError(error, L"无法确认独立回退槽，未发布新分片");
            return false;
        }
        if (manifest_status == ShardReadStatus::Valid) current = existing;
    }
    const uint8_t target = current.source_id.empty() ? 0 : static_cast<uint8_t>(1 - current.active_slot);
    const std::wstring target_path = SlotPath(paths, target);
    if (!MoveFileExW(temp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SetError(error, L"无法发布 V9 base");
        return false;
    }
    ShardManifest next;
    next.version = 9;
    next.generation = current.generation + 1;
    next.active_built = built_unix;
    next.active_wal_bytes = wal_bytes;
    next.active_slot = target;
    next.previous_slot = current.source_id.empty() ? static_cast<uint8_t>(1 - target) : current.active_slot;
    next.active_bytes = static_cast<uint64_t>(-1);
    HANDLE h = CreateFileW(target_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size)) next.active_bytes = static_cast<uint64_t>(size.QuadPart);
        CloseHandle(h);
    }
    next.active_crc64 = FileCrc64(target_path);
    if (next.active_bytes == static_cast<uint64_t>(-1) || !next.active_crc64) {
        SetError(error, L"无法完整读取 V9 base，未发布分片清单");
        return false;
    }
    next.source_id = source_id.empty()
        ? (current.source_id.empty() ? L"aggregate" : current.source_id)
        : source_id;
    if (!SaveShardManifest(paths.manifest, next, error)) return false;
    published = next;
    return true;
}
} // namespace pulse::index
