// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../model/drive_labels.h"

namespace pulse::app {
using model::DriveDisplayName;
using model::DriveLabelFromDisplayName;
} // namespace pulse::app

namespace pulse {
struct AppState;
void ApplyDriveLabelRename(AppState& s, const std::wstring& root, const std::wstring& label);
}
