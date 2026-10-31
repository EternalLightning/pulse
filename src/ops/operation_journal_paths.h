// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <string>
#include <vector>

namespace pulse::ops {

struct OperationJournalPaths {
    // The first path remains the legacy operations.json file and owns new work.
    // Owners receive legacy plus orphaned per-window files; secondaries receive
    // only their newly allocated path.
    std::vector<std::wstring> paths;
    std::wstring window_path;
};

OperationJournalPaths AssignOperationJournalPaths(const std::wstring& data_dir,
                                                  bool recovery_owner);

} // namespace pulse::ops
