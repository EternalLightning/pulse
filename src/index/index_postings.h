#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace pulse::index::postings {

struct Encoded {
    std::vector<uint32_t> starts;
    std::vector<uint8_t> bytes;
};

// Each bucket restarts at node zero; sorted IDs become small unsigned gaps.
inline bool Encode(std::span<const uint32_t> starts, std::span<const int32_t> ids,
                   Encoded& out) {
    out.starts.clear();
    out.bytes.clear();
    if (starts.empty() || starts.front() || starts.back() != ids.size()) return false;
    out.starts.reserve(starts.size());
    for (size_t bucket = 0; bucket + 1 < starts.size(); ++bucket) {
        if (starts[bucket] > starts[bucket + 1] || starts[bucket + 1] > ids.size()) return false;
        if (out.bytes.size() > UINT32_MAX) return false;
        out.starts.push_back(static_cast<uint32_t>(out.bytes.size()));
        uint32_t previous = 0;
        for (uint32_t p = starts[bucket]; p < starts[bucket + 1]; ++p) {
            const int32_t id = ids[p];
            if (id < 0 || static_cast<uint32_t>(id) < previous ||
                (p != starts[bucket] && static_cast<uint32_t>(id) == previous)) return false;
            uint32_t gap = static_cast<uint32_t>(id) - previous;
            previous = static_cast<uint32_t>(id);
            while (gap >= 128) {
                out.bytes.push_back(static_cast<uint8_t>((gap & 127) | 128));
                gap >>= 7;
            }
            out.bytes.push_back(static_cast<uint8_t>(gap));
        }
    }
    if (out.bytes.size() > UINT32_MAX) return false;
    out.starts.push_back(static_cast<uint32_t>(out.bytes.size()));
    return true;
}

inline bool Next(std::span<const uint8_t> bytes, uint32_t& cursor, uint32_t& id) {
    uint32_t gap = 0;
    for (uint32_t shift = 0; shift <= 28; shift += 7) {
        if (cursor >= bytes.size()) return false;
        const uint8_t byte = bytes[cursor++];
        if (shift == 28 && (byte & 0xf0)) return false;
        gap |= static_cast<uint32_t>(byte & 127) << shift;
        if (!(byte & 128)) {
            if ((shift && byte == 0) || gap > UINT32_MAX - id) return false;
            id += gap;
            return true;
        }
    }
    return false;
}

inline bool Validate(std::span<const uint32_t> starts, std::span<const uint8_t> bytes,
                     uint32_t node_count) {
    if (starts.empty() || starts.front() || starts.back() != bytes.size()) return false;
    for (size_t bucket = 0; bucket + 1 < starts.size(); ++bucket) {
        const uint32_t end = starts[bucket + 1];
        if (starts[bucket] > end || end > bytes.size()) return false;
        uint32_t cursor = starts[bucket], id = 0;
        bool first = true;
        const auto bounded = bytes.first(end);
        while (cursor < end) {
            const uint32_t previous = id;
            if (!Next(bounded, cursor, id) || id >= node_count || (!first && id <= previous))
                return false;
            first = false;
        }
    }
    return true;
}

} // namespace pulse::index::postings
