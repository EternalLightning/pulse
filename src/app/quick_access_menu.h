// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "context_menu.h"
#include "context_menu_prefs.h"
#include "places.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse::app {

inline void AppendQuickAccessMenuCommand(std::vector<ui::FluentMenuItem>& items,
                                        const PlacesCatalog& places,
                                        const std::vector<std::wstring>& paths) {
    std::erase_if(items, [](const auto& item) {
        return item.command == CmdPinQuickAccess || item.command == CmdUnpinQuickAccess;
    });
    if (paths.empty()) return;
    const bool all_pinned = std::all_of(paths.begin(), paths.end(),
        [&](const auto& path) { return places.IsQuickAccessPinned(path); });
    ui::FluentMenuItem item;
    item.command = all_pinned ? CmdUnpinQuickAccess : CmdPinQuickAccess;
    item.text = l10n::Get(all_pinned ? l10n::StringId::UnpinQuickAccess
                                    : l10n::StringId::PinQuickAccess);
    item.glyph = L"\xE718";
    const auto at = std::find_if(items.begin(), items.end(), [](const auto& value) {
        return value.command == CmdPinWorkspace;
    });
    items.insert(at, std::move(item));
}

} // namespace pulse::app
