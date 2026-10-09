#include "builtin_menu_items.h"
#include "builtin_menu_strings.h"
#include <array>

namespace pulse::model {
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

} // namespace pulse::model
