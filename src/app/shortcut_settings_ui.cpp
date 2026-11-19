// SPDX-License-Identifier: Apache-2.0
#include "shortcut_settings_ui.h"
#include "app_internal.h"
#include "command_catalog.h"
#include "../ui/input_dialog.h"
#include <algorithm>
#include <cwctype>

namespace pulse::app {
namespace {
std::wstring GroupName(CommandAvailability availability) {
    using A = CommandAvailability;
    switch (availability) {
    case A::Entries: return l10n::Pick(L"选择", L"Selection");
    case A::View: return l10n::Pick(L"视图", L"View");
    case A::Always: return l10n::Pick(L"窗口与工具", L"Window and tools");
    default: return l10n::Pick(L"文件操作", L"File operations");
    }
}
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
}

std::vector<ui::ShortcutSettingsRow> BuildShortcutSettingsRows(
    std::span<const CommandShortcutOverride> overrides, std::wstring_view filter) {
    std::vector<ui::ShortcutSettingsRow> rows;
    const auto needle = Lower(std::wstring(filter));
    const std::wstring groups[] = {l10n::Pick(L"文件操作", L"File operations"),
        l10n::Pick(L"选择", L"Selection"), l10n::Pick(L"视图", L"View"),
        l10n::Pick(L"窗口与工具", L"Window and tools")};
    for (const auto& group : groups) for (const auto& descriptor : GetCommandCatalog()) {
        if (GroupName(descriptor.availability) != group) continue;
        ui::ShortcutSettingsRow row;
        row.command = descriptor.command;
        row.title = l10n::Get(descriptor.label);
        row.binding = CommandShortcutText(EffectiveCommandShortcut(descriptor.command, overrides));
        row.group = group;
        row.customized = std::any_of(overrides.begin(), overrides.end(), [&](const auto& entry) {
            return entry.command == descriptor.command;
        });
        if (!needle.empty() && Lower(row.title + L" " + row.binding + L" " + row.group).find(needle) == std::wstring::npos) continue;
        rows.push_back(std::move(row));
    }
    return rows;
}

void PopulateShortcutSettings(ui::WindowViewModel& vm, const AppPrefs& prefs, std::wstring_view filter) {
    vm.settings_shortcuts = BuildShortcutSettingsRows(prefs.command_shortcuts, filter);
    vm.settings_shortcut_filter = filter;
}
} // namespace pulse::app

namespace pulse {
namespace {
std::wstring BindingError(AppState& state, int command, const app::CommandShortcut& shortcut,
                          std::span<const app::CommandShortcutOverride> overrides) {
    if (shortcut.key && state.appPrefs.global_search_enabled) {
        const unsigned modifiers = (shortcut.ctrl ? MOD_CONTROL : 0) |
            (shortcut.shift ? MOD_SHIFT : 0) | (shortcut.alt ? MOD_ALT : 0);
        if (shortcut.key == state.appPrefs.global_search_key && modifiers == state.appPrefs.global_search_modifiers)
            return l10n::Pick(L"此组合键已用于全局搜索。", L"This combination is assigned to global search.");
    }
    int conflict = 0;
    const auto result = app::ValidateCommandShortcut(command, shortcut, overrides, &conflict);
    if (result == app::ShortcutBindingResult::Reserved)
        return l10n::Pick(L"此组合键由系统或导航保留。", L"This combination is reserved for the system or navigation.");
    if (result == app::ShortcutBindingResult::Conflict) {
        const auto* descriptor = app::FindCommandDescriptor(conflict);
        return std::wstring(l10n::Pick(L"已用于：", L"Assigned to: ")) +
            (descriptor ? l10n::Get(descriptor->label) : L"");
    }
    return result == app::ShortcutBindingResult::Success ? std::wstring() :
        std::wstring(l10n::Pick(L"请输入有效组合键。", L"Enter a valid key combination."));
}
}

void HandleShortcutSettingsAction(AppState& state, int action, int command) {
    if (action == ui::ShortcutFilter) {
        ui::InputDialogSpec spec;
        spec.title = l10n::Pick(L"搜索快捷键", L"Find shortcuts");
        spec.glyph = L"\xE721";
        spec.width = 480; spec.height = 240;
        spec.fields = {{100, l10n::Pick(L"命令或组合键", L"Command or key combination"),
            state.settings.shortcut_filter(), L"Ctrl+Shift", 92, 64}};
        const auto result = ui::ShowInputDialog(state.hwnd, spec, state.darkMode, state.accentColor);
        if (result.accepted) state.settings.SetShortcutFilter(result.values[0]);
    } else if (action == ui::ShortcutClearFilter) {
        state.settings.SetShortcutFilter({});
    } else {
        auto next = state.appPrefs.command_shortcuts;
        const auto* descriptor = app::FindCommandDescriptor(command);
        if (action != ui::ShortcutResetAll && !descriptor) return;
        if (action == ui::ShortcutResetAll) {
            next.clear();
            for (const auto& item : app::GetCommandCatalog()) {
                const auto error = BindingError(state, item.command, item.shortcut, next);
                if (!error.empty()) {
                    state.notification_toast.ShowError(state.hwnd, l10n::Pick(L"快捷键", L"Shortcuts"), error);
                    return;
                }
            }
        } else if (action == ui::ShortcutRemove) {
            if (app::SetCommandShortcut(next, command, {}) != app::ShortcutBindingResult::Success) return;
        } else if (action == ui::ShortcutReset) {
            app::ResetCommandShortcut(next, command);
            const auto error = BindingError(state, command, descriptor->shortcut, next);
            if (!error.empty()) {
                state.notification_toast.ShowError(state.hwnd, l10n::Get(descriptor->label), error);
                return;
            }
        } else if (action == ui::ShortcutEdit) {
            ui::InputDialogSpec spec;
            spec.title = l10n::Get(descriptor->label);
            spec.glyph = L"\xE765";
            spec.width = 480; spec.height = 240;
            spec.fields = {{100, l10n::Pick(L"快捷键", L"Shortcut"),
                app::CommandShortcutText(app::EffectiveCommandShortcut(command, next)), L"Ctrl+Shift+K", 92, 64}};
            spec.validate = [&](const std::vector<std::wstring>& values) {
                app::CommandShortcut shortcut;
                if (!app::ParseCommandShortcut(values[0], shortcut))
                    return ui::InputDialogValidation{false, l10n::Pick(L"请输入有效组合键。", L"Enter a valid key combination.")};
                const auto error = BindingError(state, command, shortcut, state.appPrefs.command_shortcuts);
                return ui::InputDialogValidation{error.empty(), error};
            };
            const auto result = ui::ShowInputDialog(state.hwnd, spec, state.darkMode, state.accentColor);
            if (!result.accepted) return;
            app::CommandShortcut shortcut;
            if (!app::ParseCommandShortcut(result.values[0], shortcut)) return;
            next = state.appPrefs.command_shortcuts;
            const auto error = BindingError(state, command, shortcut, next);
            if (!error.empty()) {
                state.notification_toast.ShowError(state.hwnd, spec.title, error);
                return;
            }
            if (app::SetCommandShortcut(next, command, shortcut) != app::ShortcutBindingResult::Success) return;
        } else return;
        // Saving affects command bindings only; toolbar configuration has its own page.
        const auto previous = state.appPrefs.command_shortcuts;
        state.appPrefs.command_shortcuts = std::move(next);
        if (!state.appPrefs.Save()) {
            state.appPrefs.command_shortcuts = previous;
            state.notification_toast.ShowError(state.hwnd, l10n::Pick(L"快捷键", L"Shortcuts"),
                l10n::Pick(L"快捷键保存失败。", L"Could not save shortcuts."));
            return;
        }
        app::ConfigureCommandShortcuts(state.appPrefs.command_shortcuts);
        state.core.windows.ForEach([](HWND hwnd, AppState&) { InvalidateRect(hwnd, nullptr, FALSE); });
    }
    state.hitTestVmValid = false;
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
} // namespace pulse
