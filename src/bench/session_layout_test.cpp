// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "session_layout_cases.h"

int main() {
    return pulse::bench::RunSessionLayoutTests() ? 0 : 1;
}
