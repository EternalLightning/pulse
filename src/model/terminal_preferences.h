// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace pulse::model {

struct TerminalPreferences {
    static constexpr int kDefaultSize = 10;
    static constexpr int kMinSize = 8;
    static constexpr int kMaxSize = 32;

    std::wstring font_family;
    int font_size = kDefaultSize; // points
    bool follow_directory = true;

    bool operator==(const TerminalPreferences&) const = default;
};

} // namespace pulse::model
