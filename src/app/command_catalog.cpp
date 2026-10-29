#include "command_catalog.h"
#include "context_menu.h"

namespace pulse::app {
namespace {
using A = CommandAvailability;
using S = l10n::StringId;
using P = ui::fluent::MenuPictogram;

constexpr CommandDescriptor kCommands[] = {
    {CmdCopy, S::Copy, L"\xE8C8", {'C', true}, A::Selection},
    {CmdCut, S::Cut, L"\xE8C6", {'X', true}, A::MutableSelection},
    {CmdPaste, S::Paste, L"\xE77F", {'V', true}, A::Paste},
    {CmdRename, S::Rename, L"\xE8AC", {VK_F2}, A::Rename},
    {CmdDelete, S::Delete, L"\xE74D", {VK_DELETE}, A::MutableSelection},
    {CmdProperties, S::Properties, L"\xE946", {VK_RETURN, false, false, true}, A::Properties},
    {CmdCopyPath, S::CopyPath, L"\xE71B", {'C', true, true}, A::CopyPath},
    {CmdCopyToTarget, S::CopyToTarget, L"\xE8C8", {'C', true, false, true}, A::CopyToTarget},
    {CmdMoveToTarget, S::MoveToTarget, L"\xE8C6", {'X', true, false, true}, A::MoveToTarget},
    {CmdNewFolder, S::NewFolder, L"\xE8F4", {VK_F7}, A::NewItem},
    {CmdNewFolderWithSelection, S::NewFolderWithSelection, L"\xE8F4", {}, A::GroupSelection},
    {CmdUngroupFolder, S::UngroupFolder, L"\xE8B7", {}, A::DissolveSelection},
    {CmdNewFile, S::File, L"\xE8A5", {}, A::NewItem},
    {CmdUndo, S::Undo, L"\xE7A7", {'Z', true}, A::Undo},
    {CmdBatchRename, S::BatchRename, L"\xE8AC", {'R', true, true}, A::BatchRename},
    {CmdSelectAll, S::SelectAll, L"\xE8B3", {'A', true}, A::Entries},
    {CmdInvertSelection, S::InvertSelection, L"\xE7A1", {'I', true}, A::Entries},
    {CmdSelectWildcard, S::SelectWildcard, L"\xE71C", {'A', true, true}, A::Entries},
    {CmdRefresh, S::Refresh, L"\xE72C", {VK_F5}, A::View},
    {CmdOpenTerminal, S::OpenTerminalHere, L"\xE756", {}, A::Terminal},
    {CmdPinWorkspace, S::PinWorkspace, L"\xE8B7", {}, A::PinWorkspace},
    {CmdLayoutSingle, S::LayoutSingle, L"", {'1', true}, A::Always, P::LayoutSingle},
    {CmdLayoutTwoVertical, S::LayoutVertical, L"", {'2', true}, A::Always, P::LayoutSideBySide},
    {CmdLayoutTwoHorizontal, S::LayoutHorizontal, L"", {}, A::Always, P::LayoutStacked},
    {CmdLayoutThree, S::LayoutThree, L"", {'3', true}, A::Always, P::LayoutThree},
    {CmdLayoutFourGrid, S::LayoutFour, L"", {'4', true}, A::Always, P::LayoutFour},
    {CmdViewBase, S::ViewExtraLarge, L"\xE7F4", {}, A::View},
    {CmdViewBase + 1, S::ViewLarge, L"\xE7F4", {}, A::View},
    {CmdViewBase + 2, S::MediumIcons, L"\xE7F4", {}, A::View},
    {CmdViewBase + 3, S::ViewSmall, L"\xECA5", {}, A::View},
    {CmdViewBase + 4, S::ViewList, L"\xEA37", {}, A::View},
    {CmdViewBase + 5, S::ViewDetails, L"\xE8A5", {}, A::View},
    {CmdViewBase + 6, S::ViewTiles, L"\xECA5", {}, A::View},
    {CmdViewBase + 7, S::ViewContent, L"\xE8FD", {}, A::View},
    {CmdDetailsPanel, S::DetailsPane, L"\xE700", {}, A::View},
    {CmdOperationCenter, S::OperationCenter, L"\xE9D9", {}, A::Always},
    {CmdAdvancedSearch, S::AdvancedSearch, L"\xE721", {'F', true, true}, A::Always},
    {CmdInstallFullIndex, S::EnableFullIndex, L"\xE721", {}, A::Always},
    {CmdSettings, S::Settings, L"\xE713", {}, A::Always},
    {CmdOpenRecycle, S::RecycleBin, L"\xE75C", {}, A::Always},
};

bool Enabled(CommandAvailability availability, const CommandContext& context) {
    const bool selected = context.selected_count > 0 && context.selection_filesystem &&
        !context.archive_view;
    const bool transferable = selected && !context.selection_has_drive_root;
    const bool mutable_selection = transferable && !context.read_only && !context.device_selection;
    switch (availability) {
    case A::Always: return true;
    case A::Selection: return transferable;
    case A::MutableSelection: return mutable_selection;
    case A::Rename:
        return selected && !context.read_only && !context.device_selection &&
            (!context.selection_has_drive_root || context.selected_count == 1);
    case A::BatchRename: return mutable_selection && context.selected_count >= 2;
    case A::Properties: return context.archive_view || selected;
    case A::Paste:
        return context.filesystem_folder && !context.archive_view && !context.read_only && context.can_paste;
    case A::NewItem:
        return context.filesystem_folder && !context.archive_view && !context.read_only;
    case A::Undo: return context.can_undo;
    case A::CopyToTarget:
        return transferable && context.has_target && context.target_writable;
    case A::MoveToTarget:
        return mutable_selection && context.has_target && context.target_writable;
    case A::Entries: return context.has_entries;
    case A::PinWorkspace: return context.can_pin_workspace && !context.archive_view;
    case A::Terminal: return context.terminal_available && !context.archive_view;
    case A::CopyPath: return context.copy_path_available && !context.archive_view;
    case A::View: return context.can_view;
    case A::GroupSelection: return mutable_selection && context.filesystem_folder && context.can_group_selection;
    case A::DissolveSelection: return mutable_selection && context.filesystem_folder && context.can_dissolve_selection;
    }
    return false;
}
} // namespace

std::span<const CommandDescriptor> GetCommandCatalog() { return kCommands; }

const CommandDescriptor* FindCommandDescriptor(int command) {
    for (const auto& descriptor : kCommands)
        if (descriptor.command == command) return &descriptor;
    return nullptr;
}

bool IsCommandEnabled(int command, const CommandContext& context) {
    const auto* descriptor = FindCommandDescriptor(command);
    return descriptor && Enabled(descriptor->availability, context);
}

std::wstring CommandShortcutText(const CommandShortcut& shortcut) {
    if (!shortcut.key) return {};
    std::wstring text;
    if (shortcut.ctrl) text += L"Ctrl+";
    if (shortcut.alt) text += L"Alt+";
    if (shortcut.shift) text += L"Shift+";
    if (shortcut.key >= VK_F1 && shortcut.key <= VK_F24)
        text += L"F" + std::to_wstring(shortcut.key - VK_F1 + 1);
    else if (shortcut.key == VK_RETURN) text += L"Enter";
    else if (shortcut.key == VK_DELETE) text += L"Delete";
    else text += static_cast<wchar_t>(shortcut.key);
    return text;
}

int LookupCommandShortcut(unsigned key, bool ctrl, bool shift, bool alt) {
    if (!key) return CmdNone;
    for (const auto& descriptor : kCommands) {
        const auto& shortcut = descriptor.shortcut;
        if (shortcut.key == key && shortcut.ctrl == ctrl &&
            shortcut.shift == shift && shortcut.alt == alt) return descriptor.command;
    }
    return CmdNone;
}

ui::FluentMenuItem BuildCommandMenuItem(int command, const CommandContext& context) {
    ui::FluentMenuItem item;
    item.command = command;
    const auto* descriptor = FindCommandDescriptor(command);
    item.enabled = descriptor && Enabled(descriptor->availability, context);
    if (!descriptor) return item;
    item.text = l10n::Get(descriptor->label);
    item.glyph = descriptor->glyph;
    item.shortcut = CommandShortcutText(descriptor->shortcut);
    item.pictogram = descriptor->pictogram;
    if (command == CmdViewBase) item.glyph_scale = 1.16f;
    else if (command == CmdViewBase + 2) item.glyph_scale = 0.82f;
    return item;
}

} // namespace pulse::app
