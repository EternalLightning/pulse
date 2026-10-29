#pragma once
#include "../model/builtin_menu_items.h"
#include <vector>

namespace pulse::ui { struct FluentMenuItem; }

namespace pulse::app {
struct ContextMenuPrefs;
using model::BuiltinMenuItem;
using model::kBuiltinMenuItemCount;
using model::kBuiltinMenuMask;
using model::kBuiltinGroupToggle;
using model::BuiltinMenuBit;
using model::BuiltinMenuKey;
using model::BuiltinMenuLabel;
using model::RowActionMask;
using model::BuiltinMenuOrder;
using model::BuiltinMenuSurface;
using model::BuiltinFixedRow;
using model::BuiltinMenuRow;
using model::BuiltinMenuRows;
using model::BuiltinMenuSurfaceRows;
using model::BuiltinMenuRowLabel;
using model::BuiltinMenuDefaultOrder;
using model::NormalizeBuiltinMenuOrder;
using model::BuiltinMenuOrderedRows;
using model::BuiltinMenuVisibleRow;
using model::BuiltinMenuVisibleRows;
using model::BuiltinMenuShared;
using model::BuiltinMenuPreset;
using model::BuiltinMenuPresetHidden;
using model::BuiltinMenuPresetFor;
using model::kRowActionStar;
using model::kRowActionNewTab;
using model::kRowActionMore;
using model::kRowActionsAll;
using model::kRowActionOrderShift;

// Changes presentation only. Dispatch and keyboard shortcuts remain available.
std::vector<ui::FluentMenuItem> FilterBuiltinMenuItems(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items);
std::vector<ui::FluentMenuItem> ApplyBuiltinMenuPrefs(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items,
    const BuiltinMenuOrder& order = {});
std::vector<ui::FluentMenuItem> ApplyBuiltinMenuPrefs(
    std::vector<ui::FluentMenuItem> items, const ContextMenuPrefs& prefs,
    BuiltinMenuSurface surface);

} // namespace pulse::app
