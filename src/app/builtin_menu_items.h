#pragma once
#include "../model/builtin_menu_items.h"
#include <vector>

namespace pulse::ui { struct FluentMenuItem; }

namespace pulse::app {
using model::BuiltinMenuItem;
using model::kBuiltinMenuItemCount;
using model::kBuiltinMenuMask;
using model::kBuiltinGroupToggle;
using model::BuiltinMenuBit;
using model::BuiltinMenuKey;
using model::BuiltinMenuLabel;
using model::RowActionMask;

// Changes presentation only. Dispatch and keyboard shortcuts remain available.
std::vector<ui::FluentMenuItem> FilterBuiltinMenuItems(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items);

} // namespace pulse::app
