// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/operation_elevation_adapter.h"
#include <cstdio>

int main() {
    const bool passed = pulse::ops::OperationElevationAdapter::CodecChecksForTest();
    std::printf("[%s] volume-label worker accepts only bounded labels at local drive roots, rejects ordinary operations and malformed payloads, and respects cancellation/API errors\n",
        passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
