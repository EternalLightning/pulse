// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
namespace pulse {
struct AppState;
void ShowDetailsColumnsMenu(AppState& state, POINT point, int pane_index);
}
