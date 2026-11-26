// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "file_identity_protocol.h"

namespace pulse::ipc {
enum class ShellOperationKind : uint32_t { Copy, Move, Rename, CreateFolder, CreateFile, DeletePermanent, Recycle, CopyBatch };
enum class ShellCollisionPolicy : uint32_t { Replace, KeepBoth, Fail };
// Bound one native operation and fit two extended-length paths per item in one frame.
inline constexpr size_t kShellCopyBatchItems = 64;
inline constexpr size_t kShellCopyBatchPathLength = 32767;
struct ShellOperationItem {
    std::wstring source;
    std::wstring destination;
    FileIdentity expected_identity;
    FileIdentity expected_destination_identity;
};
struct ShellOperationRequest {
    ShellOperationKind kind = ShellOperationKind::Copy;
    ShellCollisionPolicy collision = ShellCollisionPolicy::Fail;
    uint64_t owner_hwnd = 0;
    std::vector<ShellOperationItem> items;
    bool elevation_authorized = false;
};
struct ShellOperationItemResult {
    std::wstring source;
    std::wstring destination;
    HRESULT hr = E_PENDING;
    bool skipped = true;
};
struct ShellOperationResult {
    HRESULT hr = S_OK;
    bool cancelled = false;
    std::wstring failed_path;
    std::vector<ShellOperationItemResult> items;
};
inline std::vector<uint8_t> WriteShellOperation(const ShellOperationRequest& request) {
    PayloadWriter writer;
    writer.PutU32(2); writer.PutU32(static_cast<uint32_t>(request.kind));
    writer.PutU32(static_cast<uint32_t>(request.collision)); writer.PutU64(request.owner_hwnd);
    writer.PutU32(request.elevation_authorized ? 1u : 0u);
    writer.PutU32(static_cast<uint32_t>(request.items.size()));
    for (const auto& item : request.items) {
        writer.PutString(item.source); writer.PutString(item.destination);
        WriteFileIdentities(writer, item.expected_identity.Valid() ? std::vector<FileIdentity>{item.expected_identity} : std::vector<FileIdentity>{});
        WriteFileIdentities(writer, item.expected_destination_identity.Valid() ? std::vector<FileIdentity>{item.expected_destination_identity} : std::vector<FileIdentity>{});
    }
    return writer.data();
}
inline bool ReadShellOperation(const uint8_t* data, size_t size, ShellOperationRequest& request) {
    PayloadReader reader(data, size);
    ShellOperationRequest parsed;
    uint32_t version = 0, kind = 0, policy = 0, count = 0, elevation = 0;
    if (!reader.GetU32(version) || (version != 1 && version != 2) || !reader.GetU32(kind) ||
        kind > static_cast<uint32_t>(ShellOperationKind::CopyBatch) || !reader.GetU32(policy) ||
        policy > static_cast<uint32_t>(ShellCollisionPolicy::Fail) || !reader.GetU64(parsed.owner_hwnd) ||
        (version == 2 && (!reader.GetU32(elevation) || elevation > 1)) ||
        !reader.GetU32(count) || !count || count > 65536 || count > reader.remaining() / 16) return false;
    parsed.kind = static_cast<ShellOperationKind>(kind); parsed.collision = static_cast<ShellCollisionPolicy>(policy);
    parsed.elevation_authorized = elevation != 0;
    for (uint32_t i = 0; i < count; ++i) {
        ShellOperationItem item;
        std::vector<FileIdentity> identities;
        std::vector<FileIdentity> destination_identities;
        if (!reader.GetString(item.source) || !reader.GetString(item.destination) ||
            !ReadFileIdentities(reader, identities) || identities.size() > 1 ||
            !ReadFileIdentities(reader, destination_identities) || destination_identities.size() > 1 ||
            item.source.find(L'\0') != std::wstring::npos || item.destination.find(L'\0') != std::wstring::npos) return false;
        const bool creating = parsed.kind == ShellOperationKind::CreateFolder || parsed.kind == ShellOperationKind::CreateFile;
        const bool deleting = parsed.kind == ShellOperationKind::DeletePermanent || parsed.kind == ShellOperationKind::Recycle;
        if ((!creating && item.source.empty()) || (!deleting && item.destination.empty()) ||
            (creating && (!item.source.empty() || !identities.empty())) || (deleting && !item.destination.empty())) return false;
        if (!identities.empty()) item.expected_identity = identities.front();
        if (!destination_identities.empty()) item.expected_destination_identity = destination_identities.front();
        parsed.items.push_back(std::move(item));
    }
    if (reader.remaining()) return false;
    request = std::move(parsed); return true;
}
inline std::vector<uint8_t> WriteShellOperationResult(const ShellOperationResult& result) {
    PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(result.hr)); writer.PutU32(result.cancelled ? 1u : 0u);
    writer.PutString(result.failed_path); writer.PutU32(static_cast<uint32_t>(result.items.size()));
    for (const auto& item : result.items) {
        writer.PutString(item.source); writer.PutString(item.destination);
        writer.PutU32(static_cast<uint32_t>(item.hr)); writer.PutU32(item.skipped ? 1u : 0u);
    }
    return writer.data();
}
inline bool ReadShellOperationResult(PayloadReader& reader, ShellOperationResult& result) {
    ShellOperationResult parsed;
    uint32_t hr = 0, cancelled = 0, count = 0;
    if (!reader.GetU32(hr) || !reader.GetU32(cancelled) || cancelled > 1 ||
        !reader.GetString(parsed.failed_path) || !reader.GetU32(count) || count > 65536 ||
        count > reader.remaining() / 16 || parsed.failed_path.find(L'\0') != std::wstring::npos) return false;
    parsed.hr = static_cast<HRESULT>(hr); parsed.cancelled = cancelled != 0;
    for (uint32_t i = 0; i < count; ++i) {
        ShellOperationItemResult item;
        uint32_t skipped = 0;
        if (!reader.GetString(item.source) || !reader.GetString(item.destination) || !reader.GetU32(hr) ||
            !reader.GetU32(skipped) || skipped > 1 || item.source.find(L'\0') != std::wstring::npos ||
            item.destination.find(L'\0') != std::wstring::npos) return false;
        item.hr = static_cast<HRESULT>(hr); item.skipped = skipped != 0;
        parsed.items.push_back(std::move(item));
    }
    if (reader.remaining()) return false;
    result = std::move(parsed); return true;
}

// Keep both the per-frame item limit and transport byte limit at the trust boundary.
// Only the final frame carries the operation outcome; earlier frames retain all child outcomes.
template<class Send>
bool SendShellOperationResultBatches(const ShellOperationResult& result, Send&& send) {
    constexpr size_t kBatchBytes = 1024 * 1024;
    constexpr size_t kBatchItems = 4096;
    ShellOperationResult batch;
    size_t bytes = 16;
    for (const auto& item : result.items) {
        const size_t item_bytes = 16 + sizeof(wchar_t) * (item.source.size() + item.destination.size());
        if (item_bytes > kMaxPayload - 16) return false;
        if (!batch.items.empty() && (batch.items.size() == kBatchItems || bytes + item_bytes > kBatchBytes)) {
            if (!send(false, WriteShellOperationResult(batch))) return false;
            batch.items.clear(); bytes = 16;
        }
        batch.items.push_back(item); bytes += item_bytes;
    }
    if (!batch.items.empty() && !send(false, WriteShellOperationResult(batch))) return false;
    batch.items.clear(); batch.hr = result.hr; batch.cancelled = result.cancelled;
    batch.failed_path = result.failed_path;
    if (batch.failed_path.size() > (kMaxPayload - 16) / sizeof(wchar_t)) return false;
    return send(true, WriteShellOperationResult(batch));
}
}
