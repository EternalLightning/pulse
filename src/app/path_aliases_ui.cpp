// SPDX-License-Identifier: Apache-2.0
#include "path_aliases_ui.h"
#include "app_internal.h"
#include "session.h"
#include "../model/path_alias_store.h"
#include "../ui/input_dialog.h"
#include <mutex>

namespace pulse {
namespace {
std::mutex windows_mutex;
std::vector<HWND> windows;
model::PathAliasStore& Store() { static model::PathAliasStore store; return store; }
void Error(AppState& s, const std::wstring& text) {
    s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::PathAliases), text);
}
std::wstring ErrorText(model::PathAliasError error) {
    using E = model::PathAliasError;
    switch (error) {
    case E::Duplicate: return l10n::Pick(L"该别名已存在。", L"This alias already exists.");
    case E::InvalidTarget: return l10n::Pick(L"请输入绝对文件夹路径。", L"Enter an absolute folder path.");
    case E::Unknown: return l10n::Pick(L"找不到该路径别名。", L"This path alias does not exist.");
    case E::InvalidSuffix: return l10n::Pick(L"别名后只能跟子目录。", L"Only subfolders can follow an alias.");
    default: return l10n::Pick(L"请输入有效别名，不包含路径分隔符。", L"Enter a valid alias without path separators.");
    }
}
bool Ready(AppState& s, const model::PathAliasSnapshot& snapshot) {
    using S = model::PathAliasStoreState;
    if (snapshot.state == S::Ready || snapshot.state == S::SaveFailed) return true;
    Error(s, snapshot.state == S::Unreadable
        ? l10n::Pick(L"别名文件无法读取，未覆盖原文件。", L"The alias file could not be read; it has been preserved.")
        : l10n::Pick(L"正在读取或保存路径别名，请稍候。", L"Path aliases are loading or saving. Please wait."));
    return false;
}
void Edit(AppState& s, const std::wstring& name, const std::wstring& target) {
    const auto snapshot = Store().Snapshot();
    if (!Ready(s, snapshot)) return;
    ui::InputDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::PathAliases); spec.glyph = L"\xE71B";
    spec.width = 560; spec.height = 310;
    spec.fields = {{100, l10n::Pick(L"别名", L"Alias"), name, L"work", 90, 64},
                   {102, l10n::Pick(L"文件夹", L"Folder"), target, L"C:\\Projects", 158, 32767}};
    spec.validate = [snapshot, name](const std::vector<std::wstring>& values) {
        auto aliases = snapshot.aliases;
        const auto error = aliases.Set(values[0], values[1], name);
        return ui::InputDialogValidation{error == model::PathAliasError::None,
            error == model::PathAliasError::None ? std::wstring{} : ErrorText(error)};
    };
    const auto result = ui::ShowInputDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (!result.accepted) return;
    auto aliases = snapshot.aliases;
    const auto error = aliases.Set(result.values[0], result.values[1], name);
    if (error != model::PathAliasError::None) { Error(s, ErrorText(error)); return; }
    if (!Store().Commit(std::move(aliases), snapshot.revision))
        Error(s, l10n::Pick(L"别名已在其他窗口更改，请重新打开。", L"Aliases changed in another window. Please reopen the editor."));
}
}
void InitializePathAliases(AppState& s) {
    if (s.isolatedTest || s.shot.active || s.menushot) return;
    {
        std::lock_guard lock(windows_mutex);
        std::erase_if(windows, [](HWND hwnd) { return !IsWindow(hwnd); });
        windows.push_back(s.hwnd);
    }
    Store().Start(app::GetPulseDataDir() + L"\\aliases.json", [] {
        std::lock_guard lock(windows_mutex);
        for (const auto hwnd : windows) PostMessageW(hwnd, WM_PATH_ALIASES_CHANGED, 0, 0);
    });
}
void HandlePathAliasesChanged(AppState& s) {
    if (Store().Snapshot().state == model::PathAliasStoreState::SaveFailed)
        Error(s, l10n::Pick(L"路径别名保存失败，原文件已保留。", L"Path aliases could not be saved; the original file was preserved."));
}
void SetPathAlias(AppState& s, const std::wstring& path) {
    if (model::PathAliases::ValidTarget(path)) Edit(s, {}, path);
}
void ShowPathAliases(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto snapshot = Store().Snapshot();
    if (!Ready(s, snapshot)) return;
    POINT point{}; GetCursorPos(&point);
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem add; add.command = 1;
    add.text = l10n::Pick(L"添加路径别名…", L"Add path alias…"); add.separator_after = true;
    items.push_back(std::move(add));
    for (size_t i = 0; i < snapshot.aliases.Items().size(); ++i) {
        const auto& alias = snapshot.aliases.Items()[i];
        ui::FluentMenuItem item; item.command = static_cast<int>(i) + 2;
        item.text = L"@" + alias.name; item.shortcut = alias.target;
        items.push_back(std::move(item));
    }
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int choice = s.menu->TrackPopup(point, std::move(items));
    if (choice == 1) {
        const auto target = ActiveTab(s) ? ActiveTab(s)->current_path : std::wstring{};
        Edit(s, {}, model::PathAliases::ValidTarget(target) ? target : std::wstring{});
        return;
    }
    const auto index = static_cast<size_t>(choice - 2);
    if (choice < 2 || index >= snapshot.aliases.Items().size()) return;
    const auto alias = snapshot.aliases.Items()[index];
    std::vector<ui::FluentMenuItem> actions;
    for (const auto& entry : {std::pair{1, l10n::Pick(L"打开", L"Open")},
                              std::pair{2, l10n::Pick(L"编辑…", L"Edit…")},
                              std::pair{3, l10n::Pick(L"移除", L"Remove")}}) {
        ui::FluentMenuItem item; item.command = entry.first; item.text = entry.second; actions.push_back(std::move(item));
    }
    const int action = s.menu->TrackPopup(point, std::move(actions));
    if (action == 1) NavigateTo(s, alias.target);
    else if (action == 2) Edit(s, alias.name, alias.target);
    else if (action == 3) {
        auto aliases = snapshot.aliases; aliases.Remove(alias.name);
        if (!Store().Commit(std::move(aliases), snapshot.revision))
            Error(s, l10n::Pick(L"别名已更改，请重新打开。", L"Aliases changed. Please reopen the menu."));
    }
}
bool ResolvePathAlias(AppState& s, const std::wstring& input, std::wstring& resolved) {
    const auto first = input.find_first_not_of(L" \t");
    if (first == std::wstring::npos || input[first] != L'@') return false;
    resolved.clear();
    const auto snapshot = Store().Snapshot();
    if (!Ready(s, snapshot)) return true;
    const auto error = snapshot.aliases.Resolve(input.substr(first), resolved);
    if (error != model::PathAliasError::None) Error(s, ErrorText(error));
    return true;
}
}
