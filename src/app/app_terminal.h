// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <cstdint>

namespace pulse {

struct AppState;
namespace model { struct TerminalPreferences; }
namespace app { struct LayoutTab; }
namespace ui { class TerminalSurface; }
ui::TerminalSurface* ActiveEmbeddedTerminal(const AppState& state);
void SyncEmbeddedTerminal(AppState& state, bool layout_changed = false);
bool ConfirmEndEmbeddedTerminal(AppState& state, const app::LayoutTab& layout);
void StopEmbeddedTerminal(app::LayoutTab& layout);
void ToggleEmbeddedTerminal(AppState& state);
void SetEmbeddedTerminalPreferences(AppState& state, const model::TerminalPreferences& preferences);
uint64_t EmbeddedTerminalCaretDeadline(const AppState& state);
void ToggleDetailsPanel(AppState& state);
bool TickEmbeddedTerminal(AppState& state, uint64_t now, bool animate);
bool HandleEmbeddedTerminalMessage(AppState& state, UINT message, WPARAM wp, LPARAM lp, LRESULT& result);

} // namespace pulse
