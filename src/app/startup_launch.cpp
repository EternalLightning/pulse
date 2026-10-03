#include "startup_launch.h"
#include <windows.h>

namespace pulse::app {

std::wstring StartupCommandLine(const std::wstring& executable) {
    return executable.empty() ? std::wstring{} : L"\"" + executable + L"\" " + kStartupArgument;
}

bool StartupCommandNeedsRepair(const std::wstring& command, const std::wstring& executable) {
    if (command.empty() || executable.empty()) return false;
    const std::wstring legacy = L"\"" + executable + L"\"";
    return CompareStringOrdinal(command.data(), static_cast<int>(command.size()),
        legacy.data(), static_cast<int>(legacy.size()), TRUE) == CSTR_EQUAL;
}

} // namespace pulse::app
