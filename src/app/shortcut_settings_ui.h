// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ui/shortcut_settings_model.h"
#include "command_shortcut.h"
#include <span>
#include <string_view>
#include <vector>

namespace pulse::ui { struct WindowViewModel; }
namespace pulse::app {
struct AppPrefs;
std::vector<ui::ShortcutSettingsRow> BuildShortcutSettingsRows(
    std::span<const CommandShortcutOverride> overrides, std::wstring_view filter);
void PopulateShortcutSettings(ui::WindowViewModel& vm, const AppPrefs& prefs, std::wstring_view filter);
}
namespace pulse {
struct AppState;
void HandleShortcutSettingsAction(AppState& state, int action, int command);
}
