#include "builtin_menu_items.h"
#include "context_menu.h"
#include <algorithm>
#include <optional>

namespace pulse::app {
namespace {

std::optional<BuiltinMenuItem> ItemForCommand(int command) {
    if (command >= CmdTagBase && command < CmdTagBase + 7) return BuiltinMenuItem::Tags;
    switch (command) {
    case CmdOpenInNewTab: return BuiltinMenuItem::OpenInNewTab;
    case CmdCopyPath: case CmdCopyAsPath: return BuiltinMenuItem::CopyPath;
    case CmdOpenTerminal: return BuiltinMenuItem::Terminal;
    case CmdPinQuickAccess: case CmdUnpinQuickAccess: return BuiltinMenuItem::QuickAccess;
    case CmdPinWorkspace: return BuiltinMenuItem::PinWorkspace;
    case CmdPinNetwork: return BuiltinMenuItem::PinNetwork;
    case CmdTags: return BuiltinMenuItem::Tags;
    case CmdViewRecentChanges: return BuiltinMenuItem::RecentChanges;
    case CmdSelectAll: case CmdInvertSelection: case CmdSelectWildcard:
        return BuiltinMenuItem::SelectCommands;
    case CmdUndo: return BuiltinMenuItem::Undo;
    default: return std::nullopt;
    }
}

bool CommandVisible(int command, uint32_t hidden) {
    const auto item = ItemForCommand(command);
    return !item || !(hidden & BuiltinMenuBit(*item));
}

} // namespace

std::vector<ui::FluentMenuItem> FilterBuiltinMenuItems(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items) {
    if (!(hidden_items & kBuiltinMenuMask)) return items;
    std::vector<ui::FluentMenuItem> visible;
    visible.reserve(items.size());
    for (auto& item : items) {
        const bool submenu = !item.children.empty();
        const bool strip = !item.quick_swatches.empty();
        if (submenu) item.children = FilterBuiltinMenuItems(std::move(item.children), hidden_items);
        std::erase_if(item.quick_swatches, [&](const ui::FluentMenuSwatch& swatch) {
            return !CommandVisible(swatch.command, hidden_items);
        });
        const bool removed = !CommandVisible(item.command, hidden_items) ||
            (submenu && item.children.empty()) || (strip && item.quick_swatches.empty());
        if (removed) {
            if (item.separator_after && !visible.empty()) visible.back().separator_after = true;
            continue;
        }
        visible.push_back(std::move(item));
    }
    if (!visible.empty()) visible.back().separator_after = false;
    return visible;
}

} // namespace pulse::app
