#include "../app/startup_launch.h"
#include <cstdio>

int main() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    const std::wstring executable = L"C:\\Program Files\\Pulse\\Pulse.exe";
    check(StartupCommandLine(executable) == L"\"C:\\Program Files\\Pulse\\Pulse.exe\" --startup",
        "sign-in registration includes an explicit startup argument");
    check(StartupCommandLine(L"").empty(), "empty executable cannot register a startup command");
    check(StartupCommandNeedsRepair(L"\"c:\\PROGRAM FILES\\pulse\\pulse.exe\"", executable),
        "legacy command migration matches the same executable case-insensitively");
    for (const auto* value : {L"\"C:\\Program Files\\Pulse\\Pulse.exe\" --startup",
            L"\"C:\\Program Files\\Pulse\\Pulse.exe\" --other", L"\"D:\\Pulse\\Pulse.exe\"",
            L"\"C:\\Program Files\\Pulse\\Pulse.exe", L""})
        check(!StartupCommandNeedsRepair(value, executable),
            "migration preserves configured arguments, other copies and malformed commands");
    return failures ? 1 : 0;
}
