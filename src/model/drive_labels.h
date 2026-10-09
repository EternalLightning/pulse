#pragma once
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <string>
#include <string_view>
#include "../fs/fs_enum.h"

namespace pulse::format {

inline const std::wstring& DriveTypeText(UINT type) {
    using l10n::StringId;
    switch (type) {
    case DRIVE_FIXED: return l10n::Get(StringId::TypeLocalDrive);
    case DRIVE_REMOVABLE: return l10n::Get(StringId::TypeRemovableDrive);
    case DRIVE_REMOTE: return l10n::Get(StringId::TypeNetworkDrive);
    case DRIVE_CDROM: return l10n::Get(StringId::TypeCdDrive);
    case DRIVE_RAMDISK: return l10n::Get(StringId::TypeRamDrive);
    default: return l10n::Get(StringId::Drive);
    }
}

inline std::wstring DriveDisplayName(const fs::DirEntry& entry) {
    if (entry.drive_type != DRIVE_UNKNOWN && entry.name.size() == 2 && entry.name[1] == L':')
        return DriveTypeText(entry.drive_type) + L" (" + entry.name + L")";
    return entry.name;
}

} // namespace pulse::format

namespace pulse::model {

inline std::wstring DriveDisplayName(std::wstring_view root, std::wstring_view label) {
    const auto drive = path::StripExtendedPathPrefix(root).substr(0, 2);
    return label.empty() ? drive : std::wstring(label) + L" (" + drive + L")";
}

inline std::wstring DriveLabelFromDisplayName(std::wstring_view root, std::wstring_view name) {
    const auto suffix = L" (" + path::StripExtendedPathPrefix(root).substr(0, 2) + L")";
    return name.ends_with(suffix) ? std::wstring(name.substr(0, name.size() - suffix.size()))
                                  : std::wstring{};
}

} // namespace pulse::model

