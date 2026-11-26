// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace pulse {
struct AppState;
struct PaneToolsState;
namespace app { struct Tab; struct CommandContext; }
namespace fs { enum class DirectorySyncMode; }

constexpr UINT WM_PANE_TOOLS_RESULT = WM_APP + 81;
void FillPaneToolsCommandContext(const AppState& s, app::CommandContext& context);
void SynchronizePaneNavigation(AppState& s, app::Tab& tab);
void ResetInvalidPaneTools(AppState& s);
void ToggleLinkedBrowsing(AppState& s);
void BeginDirectorySync(AppState& s, fs::DirectorySyncMode mode);
void ApplyPaneToolsResults(AppState& s, WPARAM identity);
void ClosePaneTools(AppState& s);
}
