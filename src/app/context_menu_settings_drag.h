#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
bool PulseMenuPreviewDragMove(AppState& state, int x, int y, WPARAM buttons);
bool PulseMenuPreviewDragEnd(AppState& state, int x, int y);
bool PulseMenuPreviewDragCancel(AppState& state);
}
