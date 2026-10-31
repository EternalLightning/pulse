// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pulse::preview {

constexpr size_t kMaxMetaFileBytes = 32 * 1024 * 1024;
constexpr size_t kMaxMetaFileRecords = 100000;

inline bool ValidateMetaFileInput(const BYTE* data, size_t bytes) {
    if (!data || bytes < 18 || bytes > kMaxMetaFileBytes) return false;
    const auto word = [&](size_t offset) {
        uint16_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    const auto dword = [&](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    if (dword(0) == EMR_HEADER) {
        constexpr size_t fixed_header = 88;
        if (bytes < fixed_header || dword(4) < fixed_header || dword(4) > bytes ||
            dword(40) != ENHMETA_SIGNATURE || dword(48) != bytes ||
            dword(52) > kMaxMetaFileRecords) return false;
        const uint32_t description_chars = dword(60), description_offset = dword(64);
        if (description_chars && (description_offset < fixed_header || description_offset % 2 ||
            description_offset > dword(4) || description_chars > (dword(4) - description_offset) / 2)) return false;
        size_t records = 0;
        for (size_t offset = 0; offset < bytes;) {
            if (bytes - offset < 8 || ++records > kMaxMetaFileRecords) return false;
            const uint32_t type = dword(offset), size = dword(offset + 4);
            if (size < 8 || size % 4 || size > bytes - offset) return false;
            offset += size;
            if (type == EMR_EOF) return size >= 20 && offset == bytes && records == dword(52);
        }
        return false;
    }
    size_t header = 0;
    if (dword(0) == 0x9ac6cdd7) {
        if (bytes < 22 + 18) return false;
        uint16_t checksum = 0;
        for (size_t offset = 0; offset < 20; offset += 2) checksum ^= word(offset);
        if (checksum != word(20)) return false;
        header = 22;
    }
    if ((word(header) != 1 && word(header) != 2) || word(header + 2) != 9 ||
        (word(header + 4) != 0x100 && word(header + 4) != 0x300) ||
        (bytes - header) % 2 || dword(header + 6) != (bytes - header) / 2 ||
        word(header + 16) != 0) return false;
    const uint32_t max_record_words = dword(header + 12);
    size_t records = 0;
    for (size_t offset = header + 18; offset < bytes;) {
        if (bytes - offset < 6 || ++records > kMaxMetaFileRecords) return false;
        const uint32_t words = dword(offset);
        const uint16_t function = word(offset + 4);
        if (words < 3 || words > max_record_words || words > (bytes - offset) / 2) return false;
        offset += static_cast<size_t>(words) * 2;
        if (function == 0) return words == 3 && offset == bytes;
    }
    return false;
}

} // namespace pulse::preview
