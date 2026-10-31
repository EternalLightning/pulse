// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "app_state.h"

namespace pulse {
bool IsWindowTransferDesktopSurface(HWND hwnd);
// A covered point has a real top-level window above the desktop, even when
// that window is not a Pulse transfer target.
AppState* WindowTransferTargetAtPoint(AppState& source, POINT screen_point, bool& covered,
                                       bool ignore_source = false);
bool CancelWindowTransferDrag(AppState& state);
void BeginWindowCaptionTransfer(HWND hwnd);
bool EndWindowCaptionTransfer(HWND hwnd);
void CancelWindowCaptionTransfer(HWND hwnd);
bool TransferLayoutTab(AppState& source, size_t index, AppState& destination,
                       bool close_empty_source = true,
                       bool replace_destination_starter = false);
bool TransferPane(AppState& source, size_t pane_index, AppState& destination,
                  size_t source_layout_index = static_cast<size_t>(-1),
                  bool replace_destination_starter = false);
bool TransferLayoutTabIntoPaneArea(AppState& source, size_t index, AppState& destination);
bool TryTransferLayoutTabAtScreenPoint(AppState& source, size_t index, POINT screen_point);
bool TryMergeWindowAtScreenPoint(AppState& source, POINT screen_point);
bool TryTransferPaneAtScreenPoint(AppState& source, size_t layout_index,
                                 size_t pane_index, POINT screen_point);
} // namespace pulse
