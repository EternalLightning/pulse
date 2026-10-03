#include "builtin_menu_items.h"
#include "builtin_menu_strings.h"
#include "context_menu.h"
#include <algorithm>
#include <array>
#include <optional>

namespace pulse::app {
namespace {

struct ItemInfo {
    std::wstring_view key;
    l10n::StringId label;
};
constexpr std::array<ItemInfo, kBuiltinMenuItemCount> kItems{{
    {L"open_new_tab", l10n::StringId::OpenNewTab},
    {L"copy_path", l10n::StringId::CopyPath},
    {L"terminal", l10n::StringId::OpenTerminalHere},
    {L"quick_access", l10n::StringId::PinQuickAccess},
    {L"pin_workspace", l10n::StringId::PinWorkspace},
    {L"pin_network", l10n::StringId::PinNetwork},
    {L"tags", builtin_text::Tags},
    {L"recent_changes", l10n::StringId::ChangeView},
    {L"select", builtin_text::Selection},
    {L"undo", l10n::StringId::Undo},
    {L"row_new_tab", builtin_text::RowNewTab},
    {L"row_star", builtin_text::RowStar},
    {L"row_more", builtin_text::RowMore},
}};

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

std::wstring_view BuiltinMenuKey(BuiltinMenuItem item) {
    const auto index = static_cast<size_t>(item);
    return index < kItems.size() ? kItems[index].key : std::wstring_view{};
}

std::wstring BuiltinMenuLabel(BuiltinMenuItem item) {
    const auto index = static_cast<size_t>(item);
    return index < kItems.size() ? l10n::Get(kItems[index].label) : std::wstring{};
}

uint32_t RowActionMask(uint32_t hidden_items) {
    uint32_t visible = 7u;
    if (hidden_items & BuiltinMenuBit(BuiltinMenuItem::RowStar)) visible &= ~1u;
    if (hidden_items & BuiltinMenuBit(BuiltinMenuItem::RowNewTab)) visible &= ~2u;
    if (hidden_items & BuiltinMenuBit(BuiltinMenuItem::RowMore)) visible &= ~4u;
    return visible;
}

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
