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
