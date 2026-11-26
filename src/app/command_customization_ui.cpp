// SPDX-License-Identifier: Apache-2.0
#include "command_customization_ui.h"
#include "app_internal.h"
#include "command_catalog.h"
#include "context_menu.h"
#include "../ui/toolbar_customization.h"
#include "../ui/toolbar_layout.h"

namespace pulse {
namespace {
std::wstring ToolbarCategory(const app::CommandDescriptor& descriptor) {
    using A = app::CommandAvailability;
    if (descriptor.availability == A::NewItem)
        return l10n::Pick(L"新建", L"Create");
    if (descriptor.availability == A::Entries)
        return l10n::Pick(L"选择", L"Selection");
    if (descriptor.availability == A::View ||
        (descriptor.command >= app::CmdLayoutSingle && descriptor.command <= app::CmdLayoutFourGrid))
        return l10n::Pick(L"视图", L"View");
    switch (descriptor.availability) {
    case A::Selection: case A::MutableSelection: case A::Rename: case A::BatchRename: case A::DisplayAlias:
    case A::Properties: case A::Paste: case A::Undo: case A::CopyToTarget:
    case A::MoveToTarget: case A::CopyPath: case A::GroupSelection: case A::DissolveSelection:
        return l10n::Pick(L"编辑", L"Edit");
    default: return l10n::Pick(L"工具", L"Tools");
    }
}
}

void ShowCommandCustomization(AppState& s) {
    ui::ToolbarCustomizationSpec spec;
    spec.title = l10n::Pick(L"自定义工具栏", L"Customize toolbar");
    spec.available_label = l10n::Pick(L"可用操作", L"Available actions");
    spec.current_label = l10n::Pick(L"当前工具栏", L"Current toolbar");
    spec.add_text = l10n::Pick(L"添加 →", L"Add →");
    spec.remove_text = l10n::Pick(L"← 移除", L"← Remove");
    spec.restore_text = l10n::Get(l10n::StringId::RestoreDefaults);
    spec.save_text = l10n::Pick(L"保存", L"Save");
    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    spec.up_text = l10n::Get(l10n::StringId::MoveUp);
    spec.down_text = l10n::Get(l10n::StringId::MoveDown);
    spec.order = s.appPrefs.toolbar_order;
    spec.default_order.assign(ui::kDefaultToolbarOrder.begin(), ui::kDefaultToolbarOrder.end());
    for (const auto& descriptor : app::GetCommandCatalog())
        spec.actions.push_back({descriptor.command, l10n::Get(descriptor.label), descriptor.glyph,
            ToolbarCategory(descriptor), descriptor.pictogram});
    const auto result = ui::ShowToolbarCustomization(s.hwnd, spec, s.darkMode, s.accentColor);
    if (!result.accepted || result.order == s.appPrefs.toolbar_order) return;
    if (spec.order != s.appPrefs.toolbar_order) {
        s.notification_toast.ShowError(s.hwnd, spec.title, l10n::Pick(
            L"工具栏已在另一个窗口修改，请重新打开设置。", L"The toolbar changed in another window. Reopen customization."));
        return;
    }
    const auto previous = s.appPrefs.toolbar_order;
    s.appPrefs.toolbar_order = result.order;
    if (!s.appPrefs.Save()) {
        s.appPrefs.toolbar_order = previous;
        s.notification_toast.ShowError(s.hwnd, spec.title, l10n::Pick(L"无法保存工具栏设置。", L"Unable to save toolbar settings."));
        return;
    }
    s.core.windows.ForEach([](HWND hwnd, AppState&) { InvalidateRect(hwnd, nullptr, FALSE); });
}
} // namespace pulse
