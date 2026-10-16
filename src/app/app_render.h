#pragma once

namespace pulse { struct AppState; }

// Returns whether another animation frame is needed after this render.
bool Render(pulse::AppState& state);
