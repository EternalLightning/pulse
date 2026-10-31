// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../shell_host/host_log_line.h"

#include <cstdio>
#include <string>
#include <string_view>

int main() {
    int failures = 0;
    const auto check = [&](bool success, const char* description) {
        std::printf("[%s] %s\n", success ? "PASS" : "FAIL", description);
        if (!success) ++failures;
    };
    const auto short_line = pulse::shell::FormatHostLogLine(123, L"short diagnostic");
    check(std::string_view(short_line.bytes.data(), short_line.length) == "[123] short diagnostic\n",
        "short host log retains exact prefix and newline");
    const std::wstring long_message(2048, L'x');
    const auto long_line = pulse::shell::FormatHostLogLine(123, long_message.c_str());
    check(long_line.length == 511 && long_line.bytes[511] == '\0' &&
        std::string_view(long_line.bytes.data(), long_line.length) == "[123] " + std::string(505, 'x'),
        "oversized diagnostic exposes only bytes actually stored for WriteFile");
    const auto boundary_line = pulse::shell::FormatHostLogLine(123, std::wstring(504, L'x').c_str());
    check(boundary_line.length == 511 && boundary_line.bytes[510] == '\n' && boundary_line.bytes[511] == '\0',
        "exact-capacity diagnostic keeps its final newline without writing the terminator");
    return failures ? 1 : 0;
}
