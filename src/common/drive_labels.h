#pragma once
#include "localization.h"
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
