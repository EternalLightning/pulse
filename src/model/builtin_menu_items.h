#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::model {

enum class BuiltinMenuItem : uint8_t {
    OpenInNewTab, CopyPath, Terminal, QuickAccess, PinWorkspace, PinNetwork,
    Tags, RecentChanges, SelectCommands, Undo, RowNewTab, RowStar, RowMore, Count
};
inline constexpr int kBuiltinMenuItemCount = static_cast<int>(BuiltinMenuItem::Count);
inline constexpr uint32_t kBuiltinMenuMask = (1u << kBuiltinMenuItemCount) - 1u;
inline constexpr int kBuiltinGroupToggle = 30;
constexpr uint32_t BuiltinMenuBit(BuiltinMenuItem item) {
    const auto index = static_cast<uint32_t>(item);
    return index < static_cast<uint32_t>(kBuiltinMenuItemCount) ? 1u << index : 0u;
}
std::wstring_view BuiltinMenuKey(BuiltinMenuItem item);
std::wstring BuiltinMenuLabel(BuiltinMenuItem item);
uint32_t RowActionMask(uint32_t hidden_items);

} // namespace pulse::model
