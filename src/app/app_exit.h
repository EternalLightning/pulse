#pragma once

namespace pulse {
struct AppState;
void RequestApplicationExit(AppState& state);
bool CompleteApplicationExit(AppState& state);
}
