#pragma once

namespace pulse::ui {

struct ToolbarCommandState {
    bool archive_view = false;
    int selected_count = 0;
    bool selection_has_drive_root = false;
    bool can_paste = false;

    bool Enabled(int command_index) const noexcept {
        if (archive_view) return command_index >= 5;
        if (command_index == 2) return can_paste;
        if (command_index >= 6) return true;
        if (selected_count == 0) return false;
        if (command_index == 0 || command_index == 1 || command_index == 4)
            return !selection_has_drive_root;
        if (command_index == 3 && selection_has_drive_root) return selected_count == 1;
        return true;
    }
};

} // namespace pulse::ui
