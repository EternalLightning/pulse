// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "builtin_menu_items.h"
#include "context_menu.h"
#include <algorithm>
#include <optional>

namespace pulse::app {
namespace {

std::optional<BuiltinMenuItem> ItemForCommand(int command) {
    if (command >= CmdTagBase && command < CmdTagBase + 7) return BuiltinMenuItem::Tags;
    if (command >= CmdViewBase && command < CmdViewBase + 8) return BuiltinMenuItem::View;
    if ((command >= CmdGroupAuto && command <= CmdGroupSize) ||
        command == CmdGroupAscending || command == CmdGroupDescending) return BuiltinMenuItem::Group;
    if (command >= CmdSortName && command <= CmdFolderSortMixed) return BuiltinMenuItem::Sort;
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
    case CmdDetailsPanel: return BuiltinMenuItem::View;
    case CmdRefresh: return BuiltinMenuItem::Refresh;
    case CmdNewFolder: case CmdNewFile: case CmdNewTextFile: return BuiltinMenuItem::NewMenu;
    case CmdPaste: return BuiltinMenuItem::Paste;
    case CmdProperties: return BuiltinMenuItem::Properties;
    case CmdFolderProperties: return BuiltinMenuItem::FolderProperties;
    case CmdNewMenu: return BuiltinMenuItem::NewMenu;
    default: return std::nullopt;
    }
}

bool CommandVisible(int command, uint32_t hidden) {
    const auto item = ItemForCommand(command);
    return !item || !(hidden & BuiltinMenuBit(*item));
}

std::optional<BuiltinMenuItem> ItemForRow(const ui::FluentMenuItem& row) {
    if (const auto item = ItemForCommand(row.command)) return item;
    if (row.command != CmdNone || row.children.empty()) return std::nullopt;
    const auto owns_children = [&](BuiltinMenuItem owner) {
        return std::all_of(row.children.begin(), row.children.end(), [&](const auto& child) {
            return ItemForCommand(child.command) == owner;
        });
    };
    for (const auto owner : {BuiltinMenuItem::View, BuiltinMenuItem::Sort, BuiltinMenuItem::Group})
        if (owns_children(owner)) return owner;
    return std::nullopt;
}

void ReorderBuiltinRows(std::vector<ui::FluentMenuItem>& items, const BuiltinMenuOrder& order) {
    if (order.empty()) return;
    struct Block { size_t first, count, rank; };
    std::vector<Block> blocks;
    for (size_t first = 0; first < items.size();) {
        const auto owner = ItemForRow(items[first]);
        const auto rank = owner ? std::find(order.begin(), order.end(), *owner) : order.end();
        size_t end = first + 1;
        if (rank != order.end()) {
            while (end < items.size() && ItemForRow(items[end]) == owner) ++end;
            blocks.push_back({first, end - first, static_cast<size_t>(rank - order.begin())});
        }
        first = end;
    }
    auto sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Block& left, const Block& right) {
        return left.rank < right.rank;
    });
    bool moved = false;
    for (size_t index = 0; index < blocks.size(); ++index)
        moved |= blocks[index].first != sorted[index].first;
    if (!moved) return;
    std::vector<bool> separators;
    separators.reserve(blocks.size());
    for (const auto& block : blocks)
        separators.push_back(items[block.first + block.count - 1].separator_after);
    std::vector<ui::FluentMenuItem> reordered;
    reordered.reserve(items.size());
    size_t next = 0;
    for (size_t index = 0; index < items.size();) {
        if (next < blocks.size() && index == blocks[next].first) {
            const auto& source = sorted[next];
            for (size_t offset = 0; offset < source.count; ++offset)
                reordered.push_back(std::move(items[source.first + offset]));
            reordered.back().separator_after = separators[next];
            index += blocks[next].count;
            ++next;
        } else {
            reordered.push_back(std::move(items[index++]));
        }
    }
    items = std::move(reordered);
}

} // namespace

std::vector<ui::FluentMenuItem> ApplyBuiltinMenuPrefs(
    std::vector<ui::FluentMenuItem> items, const ContextMenuPrefs& prefs,
    BuiltinMenuSurface surface) {
    return ApplyBuiltinMenuPrefs(std::move(items), prefs.builtin_hidden, prefs.BuiltinOrder(surface));
}

std::vector<ui::FluentMenuItem> FilterBuiltinMenuItems(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items) {
    return ApplyBuiltinMenuPrefs(std::move(items), hidden_items);
}

std::vector<ui::FluentMenuItem> ApplyBuiltinMenuPrefs(
    std::vector<ui::FluentMenuItem> items, uint32_t hidden_items,
    const BuiltinMenuOrder& order) {
    if (!(hidden_items & kBuiltinMenuMask) && order.empty()) return items;
    ReorderBuiltinRows(items, order);
    std::vector<ui::FluentMenuItem> visible;
    visible.reserve(items.size());
    for (auto& item : items) {
        const bool submenu = !item.children.empty();
        const bool strip = !item.quick_swatches.empty();
        const auto owner = ItemForRow(item);
        if (submenu) item.children = ApplyBuiltinMenuPrefs(std::move(item.children), hidden_items, order);
        std::erase_if(item.quick_swatches, [&](const ui::FluentMenuSwatch& swatch) {
            return !CommandVisible(swatch.command, hidden_items);
        });
        const bool removed = (owner && (hidden_items & BuiltinMenuBit(*owner))) ||
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
