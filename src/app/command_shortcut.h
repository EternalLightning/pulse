// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace pulse::app {
struct CommandShortcut {
    unsigned key = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool operator==(const CommandShortcut&) const = default;
};

struct CommandShortcutOverride {
    int command = 0;
    CommandShortcut shortcut; // A zero key explicitly removes the default binding.
};
} // namespace pulse::app
