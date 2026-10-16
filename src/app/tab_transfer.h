#pragma once
#include "app_state.h"

namespace pulse {
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
