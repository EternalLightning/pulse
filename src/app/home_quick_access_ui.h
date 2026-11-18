// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>
namespace pulse::ui { struct WindowViewModel; }
namespace pulse::app {
struct AppPrefs;
void PopulateHomeQuickAccessSettings(ui::WindowViewModel& vm, const AppPrefs& prefs);
}
namespace pulse {
struct AppState;
bool HandleHomeQuickAccessSettingsAction(AppState& state, int action, int index = -1,
                                        std::wstring_view path = {});
}
