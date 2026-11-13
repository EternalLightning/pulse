// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "app_model.h"
#include "../fs/drive_kind.h"
#include <dbt.h>

namespace pulse::app {
inline DWORD SidebarDriveMask(const SidebarModel& sidebar) {
    DWORD mask = 0;
    for (const auto& drive : sidebar.drives) {
        const wchar_t letter = drive.is_drive ? fs::DriveLetterOf(drive.path) : 0;
        if (letter) mask |= 1u << (letter - L'A');
    }
    return mask;
}

inline bool ShouldRefreshDriveList(WPARAM event, const DEV_BROADCAST_HDR* header,
                                  DWORD logical_drives, DWORD sidebar_drives) {
    // Device-node broadcasts also cover virtual-drive removal; ignore unrelated device changes.
    if (event == DBT_DEVNODES_CHANGED) return logical_drives != sidebar_drives;
    if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE) return false;
    return !header || header->dbch_devicetype == DBT_DEVTYP_VOLUME;
}
} // namespace pulse::app
