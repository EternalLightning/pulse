// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "command_catalog.h"
#include "context_menu.h"
#include "../ui/file_view_menu.h"
#include <algorithm>
#include <cwctype>

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
    {CmdDisplayAlias, S::DisplayAliasSet, ui::command_icons::kDisplayAliasGlyph, {VK_F2, false, true}, A::DisplayAlias},
    {CmdDelete, S::Delete, L"\xE74D", {VK_DELETE}, A::MutableSelection},
    {CmdProperties, S::Properties, ui::command_icons::kPropertiesGlyph, {VK_RETURN, false, false, true}, A::Properties},
    {CmdCopyPath, S::CopyPath, ui::command_icons::kCopyPathGlyph, {'C', true, true}, A::CopyPath},
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
    {CmdPinWorkspace, S::PinWorkspace, L"\xE718", {}, A::PinWorkspace},
    {CmdLayoutSingle, S::LayoutSingle, L"", {'1', true}, A::Always, P::LayoutSingle},
    {CmdLayoutTwoVertical, S::LayoutVertical, L"", {'2', true}, A::Always, P::LayoutSideBySide},
    {CmdLayoutTwoHorizontal, S::LayoutHorizontal, L"", {}, A::Always, P::LayoutStacked},
    {CmdLayoutThree, S::LayoutThree, L"", {'3', true}, A::Always, P::LayoutThree},
    {CmdLayoutFourGrid, S::LayoutFour, L"", {'4', true}, A::Always, P::LayoutFour},
    {CmdViewBase, ui::kFileViewChoices[0].label, ui::kFileViewChoices[0].glyph, {}, A::View},
    {CmdViewBase + 1, ui::kFileViewChoices[1].label, ui::kFileViewChoices[1].glyph, {}, A::View},
    {CmdViewBase + 2, ui::kFileViewChoices[2].label, ui::kFileViewChoices[2].glyph, {}, A::View},
    {CmdViewBase + 3, ui::kFileViewChoices[3].label, ui::kFileViewChoices[3].glyph, {}, A::View},
    {CmdViewBase + 4, ui::kFileViewChoices[4].label, ui::kFileViewChoices[4].glyph, {}, A::View},
    {CmdViewBase + 5, ui::kFileViewChoices[5].label, ui::kFileViewChoices[5].glyph, {}, A::View},
    {CmdViewBase + 6, ui::kFileViewChoices[6].label, ui::kFileViewChoices[6].glyph, {}, A::View},
    {CmdViewBase + 7, ui::kFileViewChoices[7].label, ui::kFileViewChoices[7].glyph, {}, A::View},
    {CmdDetailsPanel, S::DetailsPane, L"\xE8A0", {}, A::View},
    {CmdTerminalPanel, S::TerminalPanel, ui::command_icons::kEmbeddedTerminalGlyph, {VK_F4, true, true}, A::View},
    {CmdOperationCenter, S::OperationCenter, ui::command_icons::kOperationCenterGlyph, {}, A::Always},
    {CmdAdvancedSearch, S::AdvancedSearch, ui::command_icons::kAdvancedSearchGlyph, {'F', true, true}, A::Always},
    {CmdInstallFullIndex, S::EnableFullIndex, L"\xE721", {}, A::Always},
    {CmdSettings, S::Settings, L"\xE713", {}, A::Always},
    {CmdOpenRecycle, S::RecycleBin, L"\xE75C", {}, A::Always},
    {CmdNewMenu, S::New, ui::command_icons::kNewItemGlyph, {}, A::NewItem},
    {CmdSplitMenu, S::SplitLayout, L"\xE8A7", {}, A::Always, P::LayoutSideBySide},
    {CmdLinkedBrowsing, S::LinkedBrowsing, ui::command_icons::kLinkedBrowsingGlyph, {}, A::LinkedBrowsing},
    {CmdDirectorySync, S::DirectorySync, ui::command_icons::kDirectorySyncGlyph, {}, A::DirectorySync},
    {CmdSyncLeftToRight, S::SyncLeftToRight, L"\xE72A", {}, A::DirectorySync},
    {CmdSyncRightToLeft, S::SyncRightToLeft, L"\xE72B", {}, A::DirectorySync},
    {CmdSyncNewerBothWays, S::SyncNewerBothWays, ui::command_icons::kDirectorySyncGlyph, {}, A::DirectorySync},
    {CmdColumnLayout, S::ColumnLayout, ui::command_icons::kColumnsViewGlyph, {}, A::View},
    {CmdCustomizeCommands, S::CustomizeCommands, ui::command_icons::kCustomizeCommandsGlyph, {}, A::Always},
    {CmdPathAliases, S::PathAliases, L"\xE71B", {}, A::Always},
    {CmdSetPathAlias, S::PathAliasSet, L"\xE71B", {}, A::PinWorkspace},
    {CmdLibraries, S::ShellLibraries, L"\xE8F1", {}, A::Always},
    {CmdControlPanel, S::ShellControlPanel, L"\xE713", {}, A::Always},
    {CmdApplications, S::ShellApplications, L"\xE71D", {}, A::Always},
};

std::vector<CommandShortcutOverride> shortcut_overrides;

bool SupportedShortcutKey(unsigned key) {
    return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
        (key >= VK_F1 && key <= VK_F24) || key == VK_DELETE || key == VK_INSERT ||
        key == VK_RETURN || key == VK_SPACE || key == VK_TAB || key == VK_ESCAPE ||
        key == VK_BACK || key == VK_HOME || key == VK_END || key == VK_PRIOR ||
        key == VK_NEXT || (key >= VK_LEFT && key <= VK_DOWN);
}

bool ReservedShortcut(const CommandShortcut& shortcut) {
    for (const auto& descriptor : kCommands)
        if (shortcut == descriptor.shortcut) return false;
    if (!shortcut.ctrl && !shortcut.alt)
        return shortcut.key < VK_F1 || shortcut.key > VK_F24 ||
            shortcut.key == VK_F1 || shortcut.key == VK_F4 || shortcut.key == VK_F6;
    if (shortcut.ctrl && shortcut.alt) return true; // AltGr must remain available to text input.
    if (shortcut.key == VK_TAB || shortcut.key == VK_ESCAPE || shortcut.key == VK_SPACE ||
        shortcut.key == VK_BACK || shortcut.key == VK_RETURN ||
        (shortcut.key >= VK_LEFT && shortcut.key <= VK_DOWN) || shortcut.key == VK_HOME ||
        shortcut.key == VK_END || shortcut.key == VK_PRIOR || shortcut.key == VK_NEXT ||
        shortcut.key == VK_INSERT || shortcut.key == VK_DELETE) return true;
    if (shortcut.alt && (shortcut.key == VK_F4 || shortcut.key == 'D')) return true;
    if (shortcut.ctrl &&
        (shortcut.key == 'T' || shortcut.key == 'K' || shortcut.key == 'P' ||
         (shortcut.key == 'F' && !shortcut.shift) || shortcut.key == 'D' || shortcut.key == 'W' || shortcut.key == 'L')) return true;
    return shortcut.ctrl && shortcut.shift && shortcut.key >= '1' && shortcut.key <= '7';
}

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
    case A::DisplayAlias:
        return transferable && context.selected_count == 1 &&
            !context.shell_selection && !context.device_selection && !context.recycle_selection;
    case A::Properties: return context.shell_selection || context.archive_view || selected;
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
    case A::LinkedBrowsing: return context.can_link_browsing;
    case A::DirectorySync: return context.can_sync_directories;
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
    if (context.recycle_selection) {
        if (command == CmdDelete) return !context.read_only;
        if (command == CmdProperties || command == CmdCopyPath) return true;
    }
    if (context.device_selection && context.selection_filesystem) {
        if (command == CmdCut) return context.device_cut;
        if (command == CmdDelete) return context.device_delete;
        if (command == CmdRename) return context.device_rename && context.selected_count == 1;
    }
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
    else if (shortcut.key == VK_INSERT) text += L"Insert";
    else if (shortcut.key == VK_SPACE) text += L"Space";
    else if (shortcut.key == VK_TAB) text += L"Tab";
    else if (shortcut.key == VK_ESCAPE) text += L"Esc";
    else if (shortcut.key == VK_BACK) text += L"Backspace";
    else if (shortcut.key == VK_HOME) text += L"Home";
    else if (shortcut.key == VK_END) text += L"End";
    else if (shortcut.key == VK_PRIOR) text += L"PageUp";
    else if (shortcut.key == VK_NEXT) text += L"PageDown";
    else if (shortcut.key == VK_LEFT) text += L"Left";
    else if (shortcut.key == VK_RIGHT) text += L"Right";
    else if (shortcut.key == VK_UP) text += L"Up";
    else if (shortcut.key == VK_DOWN) text += L"Down";
    else text += static_cast<wchar_t>(shortcut.key);
    return text;
}

CommandShortcut EffectiveCommandShortcut(int command, std::span<const CommandShortcutOverride> overrides) {
    for (const auto& entry : overrides)
        if (entry.command == command) return entry.shortcut;
    const auto* descriptor = FindCommandDescriptor(command);
    return descriptor ? descriptor->shortcut : CommandShortcut{};
}

CommandShortcut EffectiveCommandShortcut(int command) {
    return EffectiveCommandShortcut(command, shortcut_overrides);
}

namespace {
int FindCommandByShortcut(const CommandShortcut& shortcut, std::span<const CommandShortcutOverride> overrides,
                          int exclude = CmdNone) {
    if (!shortcut.key) return CmdNone;
    for (const auto& descriptor : kCommands) {
        if (descriptor.command != exclude && EffectiveCommandShortcut(descriptor.command, overrides) == shortcut)
            return descriptor.command;
    }
    return CmdNone;
}
}

bool HasCommandShortcut(const CommandShortcut& shortcut, std::span<const CommandShortcutOverride> overrides) {
    return FindCommandByShortcut(shortcut, overrides) != CmdNone;
}

bool ParseCommandShortcut(std::wstring_view text, CommandShortcut& shortcut) {
    std::wstring normalized;
    for (wchar_t c : text) if (!iswspace(c)) normalized += static_cast<wchar_t>(towupper(c));
    CommandShortcut parsed;
    if (normalized.empty()) { shortcut = {}; return true; }
    size_t begin = 0;
    for (;;) {
        const auto end = normalized.find(L'+', begin);
        const auto token = normalized.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (token.empty()) return false;
        if (token == L"CTRL" || token == L"CONTROL") {
            if (parsed.ctrl || parsed.key) return false;
            parsed.ctrl = true;
        } else if (token == L"SHIFT") {
            if (parsed.shift || parsed.key) return false;
            parsed.shift = true;
        } else if (token == L"ALT") {
            if (parsed.alt || parsed.key) return false;
            parsed.alt = true;
        } else {
            if (parsed.key || end != std::wstring::npos) return false;
            if (token.size() == 1) parsed.key = static_cast<unsigned>(token.front());
            else if (token.front() == 'F' && token.size() <= 3) {
                unsigned number = 0;
                for (size_t i = 1; i < token.size(); ++i) {
                    if (token[i] < '0' || token[i] > '9') return false;
                    number = number * 10 + token[i] - '0';
                }
                if (!number || number > 24) return false;
                parsed.key = VK_F1 + number - 1;
            } else {
                constexpr std::pair<std::wstring_view, unsigned> keys[] = {
                    {L"ENTER", VK_RETURN}, {L"DELETE", VK_DELETE}, {L"DEL", VK_DELETE},
                    {L"INSERT", VK_INSERT}, {L"SPACE", VK_SPACE}, {L"TAB", VK_TAB},
                    {L"ESC", VK_ESCAPE}, {L"ESCAPE", VK_ESCAPE}, {L"BACKSPACE", VK_BACK},
                    {L"HOME", VK_HOME}, {L"END", VK_END}, {L"PAGEUP", VK_PRIOR},
                    {L"PAGEDOWN", VK_NEXT}, {L"LEFT", VK_LEFT}, {L"RIGHT", VK_RIGHT},
                    {L"UP", VK_UP}, {L"DOWN", VK_DOWN},
                };
                for (const auto& key : keys) if (token == key.first) parsed.key = key.second;
            }
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    if (!SupportedShortcutKey(parsed.key)) return false;
    shortcut = parsed;
    return true;
}

ShortcutBindingResult ValidateCommandShortcut(int command, const CommandShortcut& shortcut,
    std::span<const CommandShortcutOverride> overrides, int* conflict_command) {
    if (conflict_command) *conflict_command = CmdNone;
    if (!FindCommandDescriptor(command)) return ShortcutBindingResult::Invalid;
    if (!shortcut.key) return ShortcutBindingResult::Success;
    if (!SupportedShortcutKey(shortcut.key)) return ShortcutBindingResult::Invalid;
    if (ReservedShortcut(shortcut)) return ShortcutBindingResult::Reserved;
    if (const int conflict = FindCommandByShortcut(shortcut, overrides, command); conflict != CmdNone) {
        if (conflict_command) *conflict_command = conflict;
        return ShortcutBindingResult::Conflict;
    }
    return ShortcutBindingResult::Success;
}

ShortcutBindingResult SetCommandShortcut(std::vector<CommandShortcutOverride>& overrides,
    int command, const CommandShortcut& shortcut, int* conflict_command) {
    const auto result = ValidateCommandShortcut(command, shortcut, overrides, conflict_command);
    if (result != ShortcutBindingResult::Success) return result;
    ResetCommandShortcut(overrides, command);
    overrides.push_back({command, shortcut});
    return result;
}

void ResetCommandShortcut(std::vector<CommandShortcutOverride>& overrides, int command) {
    std::erase_if(overrides, [=](const auto& entry) { return entry.command == command; });
}

void ConfigureCommandShortcuts(std::span<const CommandShortcutOverride> overrides) {
    shortcut_overrides.clear();
    for (const auto& entry : overrides) {
        if (!FindCommandDescriptor(entry.command) ||
            (entry.shortcut.key && (!SupportedShortcutKey(entry.shortcut.key) || ReservedShortcut(entry.shortcut)))) continue;
        ResetCommandShortcut(shortcut_overrides, entry.command);
        shortcut_overrides.push_back(entry);
    }
    // Validate the final map together: exchanging two defaults must survive reload.
    for (size_t i = 0; i < shortcut_overrides.size();) {
        const auto entry = shortcut_overrides[i];
        if (ValidateCommandShortcut(entry.command, entry.shortcut, shortcut_overrides) != ShortcutBindingResult::Success) {
            shortcut_overrides.erase(shortcut_overrides.begin() + i);
            i = 0;
        } else ++i;
    }
}

int LookupCommandShortcut(unsigned key, bool ctrl, bool shift, bool alt) {
    return FindCommandByShortcut({key, ctrl, shift, alt}, shortcut_overrides);
}

ui::FluentMenuItem BuildCommandMenuItem(int command, const CommandContext& context) {
    ui::FluentMenuItem item;
    item.command = command;
    const auto* descriptor = FindCommandDescriptor(command);
    item.enabled = IsCommandEnabled(command, context);
    if (!descriptor) return item;
    item.text = l10n::Get(descriptor->label);
    item.glyph = descriptor->glyph;
    item.shortcut = CommandShortcutText(EffectiveCommandShortcut(command));
    item.pictogram = descriptor->pictogram;
    if (command == CmdLinkedBrowsing) {
        item.toggle = true;
        item.checked = context.link_browsing_on;
    } else if (command == CmdTerminalPanel) {
        item.toggle = true;
        item.checked = context.terminal_panel_open;
    }
    if (command >= CmdViewBase && command < CmdViewBase + static_cast<int>(std::size(ui::kFileViewChoices)))
        item.glyph_scale = ui::kFileViewChoices[command - CmdViewBase].glyph_scale;
    return item;
}

} // namespace pulse::app
