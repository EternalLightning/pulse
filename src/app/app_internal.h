// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// app_internal.h — Aggregate declarations for Pulse window TUs.
#pragma once
#include "app_state.h"
#include "app_render.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_input.h"
#include "app_change_tracking.h"
namespace pulse {
void SuspendContentSearches(AppState& s);
void HandleHeaderDropResult(AppState& s);
std::wstring HeaderDropHint(AppState& s, const std::vector<std::wstring>& sources);
void ClearHeaderDropHint(AppState& s);
}
