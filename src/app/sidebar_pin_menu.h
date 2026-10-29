#pragma once
#include "places.h"
#include "../ui/fluent_menu.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include <optional>
#include <string_view>

namespace pulse { struct AppState; }

namespace pulse::app {

enum class SidebarPinnedKind { QuickAccess, Workspace };
enum class SidebarPinCommand { Open = 1, Unpin = 2, Rename = 3 };

struct SidebarPinnedPlace {
    SidebarPinnedKind kind;
    std::wstring path;
};

inline std::optional<SidebarPinnedPlace> ResolveSidebarPinnedPlace(
    const PlacesCatalog& places, const std::wstring& path) {
    constexpr std::wstring_view prefix = L"pulse:workspace:";
    if (path.starts_with(prefix)) {
        int index = -1;
        if (!json::ParseInteger(std::wstring_view(path).substr(prefix.size()), index) ||
            index < 0 || static_cast<size_t>(index) >= places.workspaces.size()) return {};
        return SidebarPinnedPlace{SidebarPinnedKind::Workspace, places.workspaces[index].root};
    }
    if (places.IsQuickAccessPinned(path)) return SidebarPinnedPlace{SidebarPinnedKind::QuickAccess, path};
    return {};
}

inline std::vector<ui::FluentMenuItem> BuildSidebarPinMenu(SidebarPinnedKind kind) {
    ui::FluentMenuItem open;
    open.command = static_cast<int>(SidebarPinCommand::Open);
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8B7";
    ui::FluentMenuItem unpin;
    unpin.command = static_cast<int>(SidebarPinCommand::Unpin);
    unpin.text = kind == SidebarPinnedKind::QuickAccess
        ? l10n::Get(l10n::StringId::UnpinQuickAccess)
        : l10n::Pick(L"从工作区取消固定", L"Unpin from workspace");
    unpin.glyph = L"\xE77A";
    std::vector<ui::FluentMenuItem> items;
    items.push_back(std::move(open));
    if (kind == SidebarPinnedKind::Workspace) {
        ui::FluentMenuItem rename;
        rename.command = static_cast<int>(SidebarPinCommand::Rename);
        rename.text = l10n::Get(l10n::StringId::Rename);
        rename.glyph = L"\xE8AC";
        items.push_back(std::move(rename));
    }
    items.push_back(std::move(unpin));
    return items;
}

inline bool UnpinSidebarPlace(PlacesCatalog& places, const SidebarPinnedPlace& target) {
    return target.kind == SidebarPinnedKind::Workspace
        ? places.UnpinWorkspace(target.path) : places.SetQuickAccessPinned({target.path}, false);
}

} // namespace pulse::app

namespace pulse {
bool ShowSidebarPinMenu(AppState& s, const std::wstring& path, POINT point);
}
