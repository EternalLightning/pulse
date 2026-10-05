#pragma once
#include "../common/path_utils.h"
#include <string>
#include <string_view>

namespace pulse::app {

inline std::wstring DriveDisplayName(std::wstring_view root, std::wstring_view label) {
    const auto drive = path::StripExtendedPathPrefix(root).substr(0, 2);
    return label.empty() ? drive : std::wstring(label) + L" (" + drive + L")";
}

inline std::wstring DriveLabelFromDisplayName(std::wstring_view root, std::wstring_view name) {
    const auto suffix = L" (" + path::StripExtendedPathPrefix(root).substr(0, 2) + L")";
    return name.ends_with(suffix) ? std::wstring(name.substr(0, name.size() - suffix.size()))
                                  : std::wstring{};
}

} // namespace pulse::app

namespace pulse {
struct AppState;
void ApplyDriveLabelRename(AppState& s, const std::wstring& root, const std::wstring& label);
}
