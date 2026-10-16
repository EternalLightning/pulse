#pragma once

#include <algorithm>
#include <array>
#include <cstdio>

namespace pulse::shell {
struct HostLogLine {
    std::array<char, 512> bytes{};
    size_t length = 0;
};

inline HostLogLine FormatHostLogLine(unsigned long process_id, const wchar_t* message) {
    HostLogLine line;
    const int needed = std::snprintf(line.bytes.data(), line.bytes.size(), "[%lu] %ls\n",
        process_id, message);
    if (needed > 0)
        line.length = (std::min)(static_cast<size_t>(needed), line.bytes.size() - 1);
    return line;
}
}
