#pragma once
#include "app_model.h"
#include "network_locations.h"
#include "../common/localization.h"

namespace pulse::app {
inline void AppendSystemNetworkLocations(ui::SidebarGroup& networks,
    const std::vector<SidebarEntry>& imported) {
    ui::SidebarItem browse;
    browse.label = l10n::Get(l10n::StringId::NetworkBrowseDevices);
    browse.path = L"pulse:networks";
    browse.icon_glyph = L"\xE968";
    browse.fallback_text = L"Net";
    browse.icon_color = ui::HexColor(0x38BDF8);
    networks.items.insert(networks.items.begin(), std::move(browse));
    for (const auto& entry : imported) {
        if (std::any_of(networks.items.begin(), networks.items.end(),
            [&](const auto& item) { return SameNetworkLocation(item.path, entry.path); })) continue;
        ui::SidebarItem item;
        item.label = entry.label;
        item.detail = entry.detail;
        item.path = entry.path;
        item.icon_glyph = L"\xE968";
        item.fallback_text = L"Net";
        item.icon_color = ui::HexColor(0x38BDF8);
        networks.items.push_back(std::move(item));
    }
}
}
