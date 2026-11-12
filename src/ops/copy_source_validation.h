// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "file_object_reference.h"

namespace pulse::ops {

class CopySourceValidation {
public:
    void BeginAttempt() {
        checked_ = false;
        source_ = nullptr;
    }
    bool Observe(const FileObjectReference& expected, HANDLE source, DWORD stream, bool boundary,
                 decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) {
        if (changed_) return false;
        // CopyFile2 retains the stream handle between boundaries; each new attempt
        // must still revalidate it even if Windows reuses the numeric handle.
        if (checked_ && !boundary && source_ == source && stream_ == stream) return true;
        if (!expected.Matches(source, query)) {
            changed_ = true;
            return false;
        }
        source_ = source;
        stream_ = stream;
        checked_ = true;
        return true;
    }
    bool Checked() const { return checked_; }
    bool Changed() const { return changed_; }

private:
    HANDLE source_ = nullptr;
    DWORD stream_ = 0;
    bool checked_ = false;
    bool changed_ = false;
};

} // namespace pulse::ops
