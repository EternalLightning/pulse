#pragma once
#include <cstdint>

namespace pulse {
struct AppState;
void SyncUiScheduling(AppState& s);
void StopUiScheduling(AppState& s);
bool RunUiDeadlines(AppState& s, uint64_t now);
void CompleteFilterFocus(AppState& s);
}
