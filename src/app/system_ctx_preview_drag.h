#pragma once

#include <windows.h>

namespace pulse {
struct AppState;
bool SysCtxPreviewDragMove(AppState& state, int x, int y, WPARAM buttons);
bool SysCtxPreviewDragEnd(AppState& state, int x, int y);
bool SysCtxPreviewDragCancel(AppState& state);
}
