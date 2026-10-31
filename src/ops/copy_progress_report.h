// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::ops {

class CopyProgressReporter {
public:
    template<class Report>
    bool Observe(const COPYFILE2_MESSAGE& message, ULONGLONG tick, const Report& report) {
        uint64_t transferred = 0;
        uint64_t total = 0;
        switch (message.Type) {
        case COPYFILE2_CALLBACK_CHUNK_FINISHED:
            transferred = message.Info.ChunkFinished.uliTotalBytesTransferred.QuadPart;
            total = message.Info.ChunkFinished.uliTotalFileSize.QuadPart;
            break;
        case COPYFILE2_CALLBACK_STREAM_FINISHED:
            transferred = message.Info.StreamFinished.uliTotalBytesTransferred.QuadPart;
            total = message.Info.StreamFinished.uliTotalFileSize.QuadPart;
            break;
        case COPYFILE2_CALLBACK_ERROR:
            transferred = message.Info.Error.uliTotalBytesTransferred.QuadPart;
            total = message.Info.Error.uliTotalFileSize.QuadPart;
            break;
        default:
            // Start/poll notifications have no copied-byte count. They must not
            // publish zero/zero or reset the throttle for the next finished chunk.
            return false;
        }
        if (transferred != total && tick - last_report_tick_ < 100) return false;
        last_report_tick_ = tick;
        report(transferred, total);
        return true;
    }

private:
    ULONGLONG last_report_tick_ = 0;
};

} // namespace pulse::ops
