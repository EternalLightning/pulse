// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>

namespace pulse::index {
// Same little-endian / UTF-16 layout as PayloadWriter, without a journal-sized
// staging vector. Existing readers and the atomic temp-file replacement stay unchanged.
class ChangeJournalWriter {
public:
    explicit ChangeJournalWriter(std::ostream& stream) : stream_(stream) {}
    void PutU32(uint32_t value) { Write(&value, sizeof(value)); }
    void PutU64(uint64_t value) { Write(&value, sizeof(value)); }
    void PutString(const std::wstring& value) {
        static_assert(sizeof(wchar_t) == 2);
        PutU32(static_cast<uint32_t>(value.size()));
        Write(value.data(), value.size() * sizeof(wchar_t));
    }
    bool Flush() {
        if (used_) {
            stream_.write(buffer_.data(), static_cast<std::streamsize>(used_));
            used_ = 0;
        }
        return stream_.good();
    }
private:
    void Write(const void* bytes, size_t size) {
        auto* next = static_cast<const char*>(bytes);
        while (size && stream_) {
            if (!used_ && size >= buffer_.size()) {
                stream_.write(next, static_cast<std::streamsize>(size));
                return;
            }
            const size_t count = (std::min)(size, buffer_.size() - used_);
            std::memcpy(buffer_.data() + used_, next, count);
            used_ += count; next += count; size -= count;
            if (used_ == buffer_.size() && !Flush()) return;
        }
    }
    std::ostream& stream_;
    std::array<char, 64 * 1024> buffer_;
    size_t used_ = 0;
};
}
