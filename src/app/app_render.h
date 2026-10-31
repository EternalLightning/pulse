// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

namespace pulse { struct AppState; }

// Returns whether another animation frame is needed after this render.
bool Render(pulse::AppState& state);
