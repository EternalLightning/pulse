// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../common/command_line.h"
#include "../common/path_utils.h"
#include <string_view>

namespace pulse::ops {

inline std::wstring ExpandShellCommand(std::wstring_view command, const std::wstring& path) {
    const auto quoted = QuoteWindowsArgument(pulse::path::StripExtendedPathPrefix(path));
    const auto placeholder = [](wchar_t value) {
        return value == L'1' || value == L'L' || value == L'l' ||
            value == L'V' || value == L'v' || value == L'*';
    };
    std::wstring expanded;
    expanded.reserve(command.size() + quoted.size());
    for (size_t index = 0; index < command.size();) {
        if (command[index] == L'"' && index + 3 < command.size() &&
                command[index + 1] == L'%' && placeholder(command[index + 2]) && command[index + 3] == L'"') {
            expanded += quoted;
            index += 4;
        } else if (command[index] == L'%' && index + 1 < command.size() && placeholder(command[index + 1])) {
            expanded += quoted;
            index += 2;
        } else {
            expanded += command[index++];
        }
    }
    return expanded;
}

} // namespace pulse::ops
