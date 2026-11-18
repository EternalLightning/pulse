// SPDX-License-Identifier: Apache-2.0
#include "home_quick_access_ui.h"
#include "app_internal.h"
#include "../ui/home_quick_access_settings_model.h"

namespace pulse::app {
void PopulateHomeQuickAccessSettings(ui::WindowViewModel& vm, const AppPrefs& prefs) {
    vm.settings_home_access_hidden = prefs.home_quick_access.hidden;
    vm.settings_home_access_folders.clear();
    for (const auto& path : prefs.home_quick_access.folders) {
        const size_t slash = path.find_last_of(L"\\/");
        const std::wstring title = slash == std::wstring::npos || slash + 1 == path.size() ? path : path.substr(slash + 1);
        vm.settings_home_access_folders.push_back({title, path});
    }
}
} // namespace pulse::app

namespace pulse {
bool HandleHomeQuickAccessSettingsAction(AppState& state, int action, int index, std::wstring_view path) {
    auto next = state.appPrefs.home_quick_access;
    if (action == ui::HomeAccessToggle) {
        if (index < 0 || index >= model::kHomeQuickAccessItemCount) return false;
        next.hidden ^= 1u << index;
    } else if (action == ui::HomeAccessReset) {
        next = {};
    } else if (action == ui::HomeAccessAddFolder) {
        std::wstring folder(path);
        if (folder.empty()) {
            if (!PickFolder(state.hwnd, folder, l10n::Pick(L"添加快捷访问文件夹", L"Add quick access folder"))) return false;
        }
        next = state.appPrefs.home_quick_access;
        if (!next.AddFolder(std::move(folder))) return false;
    } else if (action == ui::HomeAccessRemove || action == ui::HomeAccessMoveUp || action == ui::HomeAccessMoveDown) {
        // The path comes from the pressed control, so a reordered list cannot
        // remove or move a different folder while the pointer is held down.
        if (path.empty()) return false;
        if (action == ui::HomeAccessRemove) {
            if (!next.RemoveFolder(path)) return false;
        } else if (!next.MoveFolder(path, action == ui::HomeAccessMoveUp ? -1 : 1)) return false;
    } else return false;
    const auto previous = state.appPrefs.home_quick_access;
    if (next == previous) return false;
    state.appPrefs.home_quick_access = next;
    if (!state.appPrefs.Save()) {
        state.appPrefs.home_quick_access = previous;
        state.notification_toast.ShowError(state.hwnd, l10n::Pick(L"快捷访问", L"Quick access"),
            l10n::Pick(L"快捷访问设置保存失败。", L"Could not save quick access settings."));
        return false;
    }
    bool origin_refreshed = false;
    auto refresh = [&](AppState& window) {
        window.home_catalog.Refresh(next);
        window.hitTestVmValid = false;
        InvalidateRect(window.hwnd, nullptr, FALSE);
    };
    state.core.windows.ForEach([&](HWND, AppState& window) {
        refresh(window);
        origin_refreshed |= &window == &state;
    });
    if (!origin_refreshed) refresh(state);
    return true;
}
} // namespace pulse
