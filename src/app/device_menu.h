// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "context_menu.h"
#include <utility>

namespace pulse::app {

// The Shell section filters built-in verbs, including for device items.
inline std::vector<ui::FluentMenuItem> BuildDeviceItemMenu(
        const CommandContext& context, bool folder) {
    ui::FluentMenuItem open;
    open.command = CmdOpen;
    open.text = l10n::Get(l10n::StringId::Open);
    open.glyph = L"\xE8E5";
    open.enabled = context.selected_count > 0 && context.selection_filesystem;
    std::vector<ui::FluentMenuItem> items{std::move(open)};
    if (folder && context.selected_count == 1) {
        ui::FluentMenuItem tab;
        tab.command = CmdOpenInNewTab;
        tab.text = l10n::Get(l10n::StringId::OpenNewTab);
        tab.glyph = L"\xE8A7";
        tab.enabled = items.front().enabled;
        items.push_back(std::move(tab));
    }
    items.back().separator_after = true;
    items.push_back(BuildCommandMenuItem(CmdCopy, context));
    items.push_back(BuildCommandMenuItem(CmdProperties, context));
    return items;
}

inline std::vector<ui::FluentMenuItem> BuildDeviceBackgroundMenu(const CommandContext& context) {
    return {BuildCommandMenuItem(CmdPaste, context), BuildCommandMenuItem(CmdNewFolder, context)};
}

} // namespace pulse::app
