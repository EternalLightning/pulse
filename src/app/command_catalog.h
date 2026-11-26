// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include "command_shortcut.h"
#include <span>
#include <vector>

namespace pulse::app {

// Capabilities are supplied by the caller; catalog evaluation never probes disk
// or invokes a Shell provider on the UI thread.
struct CommandContext {
    bool recycle_selection = false;
    int selected_count = 0;
    bool archive_view = false;
    bool shell_selection = false;
    bool read_only = false;
    bool filesystem_folder = false;
    bool selection_filesystem = false;
    bool device_selection = false;
    bool device_cut = false;
    bool device_delete = false;
    bool device_rename = false;
    bool selection_has_drive_root = false;
    bool can_group_selection = false;
    bool can_dissolve_selection = false;
    bool has_entries = false;
    bool can_paste = false;
    bool can_undo = false;
    bool has_target = false;
    bool target_writable = false;
    bool can_pin_workspace = false;
    bool terminal_available = false;
    bool copy_path_available = false;
    bool can_view = true;
    bool side_by_side = false;
    bool link_browsing_on = false;
    bool can_link_browsing = false;
    bool can_sync_directories = false;
    bool terminal_panel_open = false;
};

enum class CommandAvailability {
    Always, Selection, MutableSelection, Rename, BatchRename, Properties,
    Paste, NewItem, Undo, CopyToTarget, MoveToTarget, Entries, PinWorkspace,
    Terminal, CopyPath, View, GroupSelection, DissolveSelection, DisplayAlias, LinkedBrowsing, DirectorySync,
};

struct CommandDescriptor {
    int command;
    l10n::StringId label;
    const wchar_t* glyph;
    CommandShortcut shortcut;
    CommandAvailability availability;
    ui::fluent::MenuPictogram pictogram = ui::fluent::MenuPictogram::None;
};

std::span<const CommandDescriptor> GetCommandCatalog();
const CommandDescriptor* FindCommandDescriptor(int command);
bool IsCommandEnabled(int command, const CommandContext& context);
std::wstring CommandShortcutText(const CommandShortcut& shortcut);
CommandShortcut EffectiveCommandShortcut(int command);
CommandShortcut EffectiveCommandShortcut(int command, std::span<const CommandShortcutOverride> overrides);
bool HasCommandShortcut(const CommandShortcut& shortcut, std::span<const CommandShortcutOverride> overrides);
void ConfigureCommandShortcuts(std::span<const CommandShortcutOverride> overrides);
bool ParseCommandShortcut(std::wstring_view text, CommandShortcut& shortcut);
enum class ShortcutBindingResult { Success, Invalid, Reserved, Conflict };
ShortcutBindingResult ValidateCommandShortcut(int command, const CommandShortcut& shortcut,
    std::span<const CommandShortcutOverride> overrides, int* conflict_command = nullptr);
ShortcutBindingResult SetCommandShortcut(std::vector<CommandShortcutOverride>& overrides,
    int command, const CommandShortcut& shortcut, int* conflict_command = nullptr);
void ResetCommandShortcut(std::vector<CommandShortcutOverride>& overrides, int command);
// Exact modifier match. Text editors keep their existing, higher-priority input
// handling; callers decide when a shortcut may be dispatched.
int LookupCommandShortcut(unsigned key, bool ctrl, bool shift, bool alt);
ui::FluentMenuItem BuildCommandMenuItem(int command, const CommandContext& context);

} // namespace pulse::app
