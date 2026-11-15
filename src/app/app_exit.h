// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <functional>

namespace pulse {
struct AppState;
// The hook runs after confirmation; failure keeps the application open.
void RequestApplicationExit(AppState& state, const std::function<bool()>& before_shutdown = {});
void RequestAppWindowClose(AppState& state);
void RequestWindowClose(AppState& state);
bool CompleteApplicationExit(AppState& state);
}
