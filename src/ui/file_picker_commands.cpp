// SPDX-License-Identifier: Apache-2.0
#include "file_picker_commands.h"
#include "input_dialog.h"
#include "command_icons.h"

namespace pulse::ui {

PickerCommandInfo PickerCommandPresentation(PickerCommand command) {
    using enum PickerCommand;
    using I = l10n::StringId;
    switch (command) {
    case New: return {I::New, command_icons::kNewItemGlyph};
    case Cut: return {I::Cut, L"\xE8C6", L"Ctrl+X"};
    case Copy: return {I::Copy, L"\xE8C8", L"Ctrl+C"};
    case Paste: return {I::Paste, L"\xE77F", L"Ctrl+V"};
    case Rename: return {I::Rename, L"\xE8AC", L"F2", true};
    case Recycle: return {I::OperationCenterRecycle, L"\xE74D", L"Del"};
    case Refresh: return {I::Refresh, L"\xE72C", L"F5"};
    case Open: return {I::Open, L"\xE838", L"Enter"};
    case NewFolder: return {I::NewFolder, L"\xE8F4", L"F7", true};
    case NewFile: return {I::NewFile, L"\xE8A5", L"", true};
    case NewText: return {I::NewTextDocument, L"\xE8A5", L"", true};
    case CopyPath: return {I::CopyPath, command_icons::kCopyPathGlyph, L"Ctrl+Shift+C"};
    case Properties: return {I::Properties, command_icons::kPropertiesGlyph, L"Alt+Enter"};
    case Undo: return {I::Undo, L"\xE7A7", L"Ctrl+Z"};
    default: return {I::New, L""};
    }
}

bool PickerCommandEnabled(PickerCommand command, const PickerCommandState& state) {
    using enum PickerCommand;
    if (!state.ready) return false;
    switch (command) {
    case Refresh: return true;
    case Undo: return state.can_undo;
    case Open: return state.selected == 1;
    default: break;
    }
    if (!state.in_folder) return false;
    switch (command) {
    case New: case NewFolder: case NewFile: case NewText: return true;
    case Paste: return state.has_clipboard;
    case Rename: return state.selected == 1;
    case Cut: case Copy: case Recycle: case CopyPath: case Properties: return state.selected != 0;
    default: return false;
    }
}

namespace {
FluentMenuItem Item(PickerCommand command, bool enabled = true) {
    const auto presentation = PickerCommandPresentation(command);
    FluentMenuItem item;
    item.command = static_cast<int>(command);
    item.text = l10n::Get(presentation.label);
    if (presentation.ellipsis && !item.text.ends_with(L"…")) item.text += L"…";
    item.glyph = presentation.glyph;
    item.shortcut = presentation.shortcut;
    item.enabled = enabled;
    return item;
}
}

std::vector<FluentMenuItem> PickerNewMenu() {
    using enum PickerCommand;
    return {Item(NewFolder), Item(NewFile), Item(NewText)};
}

std::vector<FluentMenuItem> PickerContextMenu(const PickerCommandState& state) {
    using enum PickerCommand;
    std::vector<FluentMenuItem> menu;
    const auto add = [&](PickerCommand command, bool separator = false) {
        auto item = Item(command, PickerCommandEnabled(command, state));
        item.separator_after = separator;
        menu.push_back(std::move(item));
    };
    if (state.selected == 1) add(Open, state.in_folder);
    if (!state.in_folder) {
        add(Refresh);
    } else if (!state.selected) {
        add(New);
        menu.back().children = PickerNewMenu();
        add(Paste, true);
        add(Refresh);
    } else {
        add(Cut);
        add(Copy);
        add(Paste, true);
        add(Rename);
        add(Recycle, true);
        add(CopyPath);
        add(Properties);
    }
    if (state.in_folder || state.can_undo) add(Undo);
    return menu;
}

std::wstring PromptPickerName(HWND owner, PickerCommand command, bool dark, D2D1_COLOR_F accent) {
    using I = l10n::StringId;
    const auto presentation = PickerCommandPresentation(command);
    InputDialogSpec spec;
    spec.window_class = L"PulsePickerItemName";
    spec.title = l10n::Get(presentation.label);
    spec.glyph = presentation.glyph;
    spec.confirm_text = l10n::Get(I::CreateItemAction);
    return PromptFileName(owner, std::move(spec), {},
        command == PickerCommand::NewText ? kNewTextDocumentNameLimit : 255, dark, accent);
}

} // namespace pulse::ui
