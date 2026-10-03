#pragma once
#include <string>

namespace pulse::app {

inline constexpr wchar_t kStartupArgument[] = L"--startup";
std::wstring StartupCommandLine(const std::wstring& executable);
bool StartupCommandNeedsRepair(const std::wstring& command, const std::wstring& executable);

} // namespace pulse::app
