#pragma once
#include "protocol.h"
#include "../common/file_identity.h"

namespace pulse::ipc {
inline void WriteFileIdentities(PayloadWriter& writer, const std::vector<FileIdentity>& identities) {
    writer.PutU32(static_cast<uint32_t>(identities.size()));
    for (const auto& identity : identities) {
        writer.PutU32(identity.extended ? 1u : 0u);
        writer.PutU64(identity.value.VolumeSerialNumber);
        uint64_t halves[2]{};
        std::memcpy(halves, identity.value.FileId.Identifier, sizeof(halves));
        writer.PutU64(halves[0]); writer.PutU64(halves[1]);
    }
}

inline bool ReadFileIdentities(PayloadReader& reader, std::vector<FileIdentity>& identities) {
    uint32_t count = 0;
    if (!reader.GetU32(count) || count > 100000 || count > reader.remaining() / 28) return false;
    std::vector<FileIdentity> parsed;
    parsed.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileIdentity identity;
        uint32_t extended = 0;
        uint64_t halves[2]{};
        if (!reader.GetU32(extended) || extended > 1 || !reader.GetU64(identity.value.VolumeSerialNumber) ||
            !reader.GetU64(halves[0]) || !reader.GetU64(halves[1])) return false;
        identity.extended = extended != 0;
        std::memcpy(identity.value.FileId.Identifier, halves, sizeof(halves));
        if (!identity.Valid()) return false;
        parsed.push_back(identity);
    }
    identities = std::move(parsed);
    return true;
}
}
