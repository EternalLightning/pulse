// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "usn_packet_queue.h"
#include "usn_record.h"

namespace pulse::index {

// A startup turn yields after four packets even if the captured journal tail is
// farther away. The normal collector resumes at the last committed checkpoint.
inline constexpr size_t kUsnCatchUpPackets = 4;

template<class Read, class Commit, class Active>
DWORD CatchUpUsnBatch(int64_t target, int64_t& cursor, Read read, Commit commit, Active active) {
    std::vector<BYTE> buffer(UsnPacketQueue::kReadBytes);
    for (size_t packet = 0; cursor < target && packet < kUsnCatchUpPackets; ++packet) {
        if (!active()) return ERROR_OPERATION_ABORTED;
        DWORD received = 0;
        const DWORD error = read(cursor, buffer.data(), static_cast<DWORD>(buffer.size()), received);
        if (error) return error;
        if (received < sizeof(cursor) || received > buffer.size()) return ERROR_INVALID_DATA;
        int64_t next = 0;
        std::memcpy(&next, buffer.data(), sizeof(next));
        if (next <= cursor) return ERROR_INVALID_DATA;
        const BYTE* records = buffer.data() + sizeof(cursor);
        const size_t bytes = received - sizeof(cursor);
        size_t offset = 0;
        int64_t previous = cursor;
        while (offset < bytes) {
            UsnRecordView view;
            if (!ReadUsnRecord(records + offset, bytes - offset, view) ||
                view.record.Usn < previous || view.record.Usn >= next) return ERROR_INVALID_DATA;
            previous = view.record.Usn;
            offset += view.record.RecordLength;
        }
        if (!active()) return ERROR_OPERATION_ABORTED;
        const DWORD applied = commit(records, bytes, next);
        if (applied) return applied;
        cursor = next;
    }
    return ERROR_SUCCESS;
}

} // namespace pulse::index
