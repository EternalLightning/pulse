// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ipc/protocol.h"
#include <string>
#include <vector>

namespace pulse::index {
inline constexpr uint32_t kFolderSizeRequest = 9, kFolderSizeResponse = 109;
inline constexpr size_t kFolderSizeBatch = 128;
struct IndexedFolderSize { bool available = false; uint64_t bytes = 0; };
inline void PutFolderSizes(ipc::PayloadWriter& w, const std::vector<IndexedFolderSize>& values) {
    w.PutU32(1); w.PutU32(static_cast<uint32_t>(values.size()));
    for (const auto& value : values) { w.PutU32(value.available ? 1u : 0u); w.PutU64(value.bytes); }
}
inline bool ReadFolderSizes(ipc::PayloadReader& r, size_t expected, std::vector<IndexedFolderSize>& values) {
    uint32_t version = 0, count = 0;
    if (!r.GetU32(version) || version != 1 || !r.GetU32(count) || count != expected || count > kFolderSizeBatch) return false;
    std::vector<IndexedFolderSize> result;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t available = 0; uint64_t bytes = 0;
        if (!r.GetU32(available) || available > 1 || !r.GetU64(bytes)) return false;
        result.push_back({available != 0, bytes});
    }
    if (r.remaining()) return false;
    values = std::move(result); return true;
}
}
