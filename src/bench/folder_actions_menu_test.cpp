// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "change_tracking_menu_test.h"
#include "folder_actions_menu_cases.h"
#include <cstdio>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    };
    RunRecentChangesMenuChecks(check);
    RunFolderActionsMenuChecks(check);
    return failures ? 1 : 0;
}
