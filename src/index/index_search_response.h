// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_protocol.h"
#include <new>
#include <stdexcept>

namespace pulse::index {

inline SearchResult SearchFailure(DWORD error) {
    SearchResult result;
    result.error = error ? error : ERROR_GEN_FAILURE;
    result.incomplete = true;
    return result;
}

inline bool DecodeSearchResponse(const uint8_t* data, size_t bytes, SearchResult& result) {
    try {
        ipc::PayloadReader reader(data, bytes);
        uint32_t total = 0, count = 0;
        constexpr size_t kMinimumHitBytes = 2 * sizeof(uint32_t) + 5 * sizeof(uint32_t);
        if (!reader.GetU32(total) || !reader.GetU32(count) || count > kSearchPageCap ||
            count > reader.remaining() / kMinimumHitBytes) return false;
        SearchResult parsed;
        parsed.total = total;
        parsed.hits.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Hit hit;
            uint32_t flags = 0;
            if (!reader.GetString(hit.path) || !reader.GetString(hit.name) ||
                !reader.GetU32(flags) || flags > 1 || !reader.GetU64(hit.size) ||
                !reader.GetU64(hit.mtime)) return false;
            hit.is_dir = flags != 0;
            parsed.hits.push_back(std::move(hit));
        }
        // Older helpers omitted quality fields; a partial trailer is invalid.
        if (reader.remaining() && reader.remaining() != 8 && reader.remaining() != 16) return false;
        if (reader.remaining() && !reader.GetU64(parsed.revision)) return false;
        if (reader.remaining()) {
            uint32_t incomplete = 0;
            if (!reader.GetU32(parsed.error) || !reader.GetU32(incomplete) || incomplete > 1) return false;
            parsed.incomplete = incomplete != 0;
        }
        result = std::move(parsed);
        return true;
    } catch (const std::bad_alloc&) {
        result = SearchFailure(ERROR_NOT_ENOUGH_MEMORY);
    } catch (const std::length_error&) {
        result = SearchFailure(ERROR_INVALID_DATA);
    }
    return false;
}

inline uint64_t SearchResponseHash(const std::vector<uint8_t>& payload) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < payload.size(); ++i) {
        // The revision precedes the error and incomplete fields.
        if (payload.size() >= 16 && i >= payload.size() - 16 && i < payload.size() - 8) continue;
        hash ^= payload[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace pulse::index
