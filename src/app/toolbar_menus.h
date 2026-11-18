// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "command_catalog.h"
#include "../ui/toolbar_layout.h"

namespace pulse::app {
inline constexpr int kUnpinToolbarItem = 901;
std::vector<ui::FluentMenuItem> BuildToolbarOverflowMenu(std::span<const int> order,
    const ui::ToolbarLayout& layout, const CommandContext& context, int split_layout);
std::vector<ui::FluentMenuItem> BuildToolbarContextMenu(bool removable);
}
