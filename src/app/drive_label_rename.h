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
