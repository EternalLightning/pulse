#pragma once

#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include <span>

namespace pulse::app {

// Capabilities are supplied by the caller; catalog evaluation never probes disk
// or invokes a Shell provider on the UI thread.
struct CommandContext {
    int selected_count = 0;
    bool archive_view = false;
    bool read_only = false;
    bool filesystem_folder = false;
    bool selection_filesystem = false;
    bool selection_has_drive_root = false;
    bool has_entries = false;
    bool can_paste = false;
    bool can_undo = false;
    bool has_target = false;
    bool target_writable = false;
    bool can_pin_workspace = false;
    bool terminal_available = false;
    bool copy_path_available = false;
    bool can_view = true;
};

enum class CommandAvailability {
    Always, Selection, MutableSelection, Rename, BatchRename, Properties,
    Paste, NewItem, Undo, CopyToTarget, MoveToTarget, Entries, PinWorkspace,
    Terminal, CopyPath, View,
};

struct CommandShortcut {
    unsigned key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
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
// Exact modifier match. Text editors keep their existing, higher-priority input
// handling; callers decide when a shortcut may be dispatched.
int LookupCommandShortcut(unsigned key, bool ctrl, bool shift, bool alt);
ui::FluentMenuItem BuildCommandMenuItem(int command, const CommandContext& context);

} // namespace pulse::app
