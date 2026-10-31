// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "app_runtime.h"

namespace pulse {
std::vector<std::wstring> QuickAccessTargets(const app::Tab* tab, bool background);
void AppendQuickAccessCommand(AppState& s, std::vector<ui::FluentMenuItem>& items,
                              const std::vector<std::wstring>& paths);
bool HandleQuickAccessCommand(AppState& s, int command,
                              const std::vector<std::wstring>& paths);
}
