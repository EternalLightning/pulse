// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ui/ui_renderer.h"

namespace pulse::app {
struct SettingsInputState {
    ui::HitTestResult focus, pressed;
    uint64_t tab = 0;
    int page = -1;
    bool keyboard = false;
};
}

namespace pulse {
struct AppState;
bool ActivateSettingsControl(AppState& state, const ui::HitTestResult& hit, bool keyboard = false);
bool BeginSettingsPointer(AppState& state, const ui::HitTestResult& hit);
bool EndSettingsPointer(AppState& state, int x, int y);
void CancelSettingsPointer(AppState& state);
bool HandleSettingsKey(AppState& state, WPARAM key, LPARAM flags);
POINT SettingsPopupPoint(AppState& state);
}
