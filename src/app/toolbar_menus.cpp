// SPDX-License-Identifier: Apache-2.0
#include "toolbar_menus.h"
#include "context_menu.h"

namespace pulse::app {
std::vector<ui::FluentMenuItem> BuildToolbarOverflowMenu(std::span<const int> order,
    const ui::ToolbarLayout& layout, const CommandContext& context, int split_layout) {
    std::vector<ui::FluentMenuItem> items;
    for (const int command : order) {
        if (command == CmdCustomizeCommands || !FindCommandDescriptor(command) ||
            std::any_of(layout.placed_commands.begin(), layout.placed_commands.end(),
                [=](const auto& item) { return item.command == command; }) ||
            std::any_of(items.begin(), items.end(),
                [=](const auto& item) { return item.command == command; })) continue;
        auto item = BuildCommandMenuItem(command, context);
        if (command == CmdNewMenu) item.children = BuildNewMenu();
        if (command == CmdSplitMenu) item.children = BuildSplitMenu(split_layout);
        items.push_back(std::move(item));
    }
    if (!items.empty()) items.back().separator_after = true;
    items.push_back(BuildCommandMenuItem(CmdCustomizeCommands, context));
    return items;
}
std::vector<ui::FluentMenuItem> BuildToolbarContextMenu(bool removable) {
    std::vector<ui::FluentMenuItem> items;
    if (removable) {
        ui::FluentMenuItem remove;
        remove.command = kUnpinToolbarItem;
        remove.text = l10n::Pick(L"移除此按钮", L"Remove this button");
        remove.glyph = L"\xE711";
        remove.separator_after = true;
        items.push_back(std::move(remove));
    }
    items.push_back(BuildCommandMenuItem(CmdCustomizeCommands, {}));
    return items;
}
}
