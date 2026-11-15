// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "ops_manager.h"

namespace pulse::ops {

// Only volume labels need a separate worker; IFileOperation owns file-operation elevation.
class OperationElevationAdapter {
public:
    static void RunParent(OpsManager& manager, const OpsManager::QueueItem& item);
    static int RunWorker(int argc, wchar_t** argv);
#ifdef PULSE_OPERATION_ELEVATION_TESTING
    static bool CodecChecksForTest();
#endif
};

} // namespace pulse::ops
