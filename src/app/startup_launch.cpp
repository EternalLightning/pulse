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

void CaptureStartupOpenPath(std::wstring_view argument, bool last, std::wstring& path) {
    if (argument.empty() || argument.front() == L'-' ||
        CompareStringOrdinal(argument.data(), static_cast<int>(argument.size()),
            L"/Embedding", -1, TRUE) == CSTR_EQUAL ||
        CompareStringOrdinal(argument.data(), static_cast<int>(argument.size()),
            L"/Automation", -1, TRUE) == CSTR_EQUAL) return;
    if (last || path.empty()) path.assign(argument);
}

} // namespace pulse::app
