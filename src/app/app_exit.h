#pragma once

namespace pulse {
struct AppState;
void RequestApplicationExit(AppState& state);
void RequestWindowClose(AppState& state);
bool CompleteApplicationExit(AppState& state);
}
