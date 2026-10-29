#include "sidebar_pin_menu.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "settings_value_dialog.h"

namespace pulse {

bool ShowSidebarPinMenu(AppState& s, const std::wstring& path, POINT point) {
    // Keep the root identity across the popup's nested message loop; removing
    // another workspace can change its old numeric sidebar index.
    const auto target = app::ResolveSidebarPinnedPlace(s.places, path);
    if (!target) return false;
    if (!EnsureMenu(s)) return true;
    const auto command = static_cast<app::SidebarPinCommand>(
        s.menu->TrackPopup(point, app::BuildSidebarPinMenu(target->kind)));
    if (command == app::SidebarPinCommand::Open) {
        if (target->kind == app::SidebarPinnedKind::Workspace) {
            const int index = s.places.FindWorkspace(target->path);
            if (index >= 0) OpenWorkspace(s, index);
        } else {
            NavigateTo(s, target->path);
        }
    } else if (command == app::SidebarPinCommand::Unpin) {
        app::UnpinSidebarPlace(s.places, *target);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    } else if (command == app::SidebarPinCommand::Rename &&
               target->kind == app::SidebarPinnedKind::Workspace) {
        const int index = s.places.FindWorkspace(target->path);
        if (index < 0) return true;
        const auto name = s.places.workspaces[static_cast<size_t>(index)].name;
        std::wstring renamed;
        if (app::PromptSettingsValue(s.hwnd,
            l10n::Pick(L"重命名工作区", L"Rename workspace"),
            l10n::Pick(L"工作区名称", L"Workspace name"), name, renamed, 128,
            l10n::Get(l10n::StringId::Rename), s.darkMode, s.accentColor)) {
            s.places.RenameWorkspace(target->path, renamed);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
    }
    return true;
}

} // namespace pulse
