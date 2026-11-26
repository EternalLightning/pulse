// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../ui/file_picker_dialog.h"

namespace pulse {
struct AppState;

namespace app {
struct AppPrefs;
void SetFilePickerFolderViews(ui::FilePickerSpec& spec, const AppPrefs& prefs);
}

bool PickPulsePath(AppState& state, HWND owner, ui::FilePickerSpec spec, std::wstring& path,
                   const std::function<bool(const std::wstring&)>& validate = {});
} // namespace pulse
