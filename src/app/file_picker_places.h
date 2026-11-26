// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../ui/file_picker_dialog.h"

namespace pulse::app {

ui::PickerPlaces LoadFilePickerPlaces(const std::function<bool()>& cancelled);
ui::PickerPlaces LoadTakeoverPickerPlaces(const std::function<bool()>& cancelled);

} // namespace pulse::app
