// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::model {

enum class BuiltinMenuItem : uint8_t {
    OpenInNewTab, CopyPath, Terminal, QuickAccess, PinWorkspace, PinNetwork,
    Tags, RecentChanges, SelectCommands, Undo, RowNewTab, RowStar, RowMore,
    View, Sort, Group, Refresh, NewFolder, NewTextFile, Paste, Properties,
    FolderProperties, NewMenu, Count
};
inline constexpr int kBuiltinMenuItemCount = static_cast<int>(BuiltinMenuItem::Count);
static_assert(kBuiltinMenuItemCount < 32, "builtin_hidden is a 32-bit mask");
inline constexpr uint32_t kBuiltinMenuMask = (1u << kBuiltinMenuItemCount) - 1u;
inline constexpr int kBuiltinGroupToggle = 30;
constexpr uint32_t BuiltinMenuBit(BuiltinMenuItem item) {
    const auto index = static_cast<uint32_t>(item);
    return index < static_cast<uint32_t>(kBuiltinMenuItemCount) ? 1u << index : 0u;
}
std::wstring_view BuiltinMenuKey(BuiltinMenuItem item);
std::wstring BuiltinMenuLabel(BuiltinMenuItem item);

using BuiltinMenuOrder = std::vector<BuiltinMenuItem>;
enum class BuiltinMenuSurface : uint8_t { Item, Background, RowButtons, Count };
enum class BuiltinFixedRow : uint8_t { None, Open, Strip };

struct BuiltinMenuRow {
    BuiltinMenuItem item;
    BuiltinFixedRow fixed;
    const wchar_t* glyph;
    const wchar_t* shortcut;
    bool separator_after;
    bool submenu;
};
struct BuiltinMenuRows {
    const BuiltinMenuRow* rows;
    size_t count;
};
BuiltinMenuRows BuiltinMenuSurfaceRows(BuiltinMenuSurface surface);
std::wstring BuiltinMenuRowLabel(const BuiltinMenuRow& row);
BuiltinMenuOrder BuiltinMenuDefaultOrder(BuiltinMenuSurface surface);
// Drops foreign and repeated rows, then restores missing rows after their
// default predecessor so newly introduced items have a stable position.
BuiltinMenuOrder NormalizeBuiltinMenuOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& saved);
// Separators stay with their slots when rows move.
std::vector<BuiltinMenuRow> BuiltinMenuOrderedRows(BuiltinMenuSurface surface, const BuiltinMenuOrder& order);
struct BuiltinMenuVisibleRow {
    BuiltinMenuRow row;
    bool separator_after;
};
std::vector<BuiltinMenuVisibleRow> BuiltinMenuVisibleRows(BuiltinMenuSurface surface,
    uint32_t hidden_items, const BuiltinMenuOrder& order = {});
bool BuiltinMenuShared(BuiltinMenuItem item);

enum class BuiltinMenuPreset : uint8_t { Slim, Standard, Full, Count };
uint32_t BuiltinMenuPresetHidden(BuiltinMenuPreset preset);
BuiltinMenuPreset BuiltinMenuPresetFor(uint32_t hidden_items);

inline constexpr uint32_t kRowActionStar = 1u;
inline constexpr uint32_t kRowActionNewTab = 2u;
inline constexpr uint32_t kRowActionMore = 4u;
inline constexpr uint32_t kRowActionsAll = 7u;
// Bits 3-8 hold three left-to-right 2-bit ids: star=0, new tab=1, more=2.
// Zero means the default renderer order (new tab, star, more).
inline constexpr uint32_t kRowActionOrderShift = 3u;
uint32_t RowActionMask(uint32_t hidden_items, const BuiltinMenuOrder& row_order = {});

} // namespace pulse::model
