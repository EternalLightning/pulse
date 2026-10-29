#pragma once
#include <cstdint>
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }
bool TickAppSurfaceScroll(AppState& s, ui::WindowViewModel& vm, uint64_t now);
void CancelAppSurfaceScroll(AppState& s);
void StartHorizontalSmoothScroll(AppState& s, float delta, float maximum, float viewport, uint64_t now);
}
