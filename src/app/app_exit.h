#pragma once

namespace pulse {
struct AppState;
void RequestApplicationExit(AppState& state);
void RequestAppWindowClose(AppState& state);
void RequestWindowClose(AppState& state);
bool CompleteApplicationExit(AppState& state);
}
