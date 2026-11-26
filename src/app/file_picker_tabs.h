// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../ui/file_picker_dialog.h"

namespace pulse { struct AppState; }

namespace pulse::app {

inline constexpr ULONG_PTR kPickerTabsRequest = 0x50554C54; // 'PULT'

bool PublishPickerTabs(const AppState& state);
std::vector<ui::PickerPlace> ReadPickerTabs(HWND source);

} // namespace pulse::app
