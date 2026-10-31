// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>

namespace pulse {
struct AppState;
void SyncUiScheduling(AppState& s);
void StopUiScheduling(AppState& s);
bool RunUiDeadlines(AppState& s, uint64_t now);
void CompleteFilterFocus(AppState& s);
}
