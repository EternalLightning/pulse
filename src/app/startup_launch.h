#pragma once
#include <string>
#include <string_view>

namespace pulse::app {

inline constexpr wchar_t kStartupArgument[] = L"--startup";
std::wstring StartupCommandLine(const std::wstring& executable);
bool StartupCommandNeedsRepair(const std::wstring& command, const std::wstring& executable);
void CaptureStartupOpenPath(std::wstring_view argument, bool last, std::wstring& path);

} // namespace pulse::app
