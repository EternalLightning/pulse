// SPDX-License-Identifier: Apache-2.0
#include "file_picker_places.h"

#include "app_model.h"
#include "file_picker_tabs.h"
#include "network_locations.h"
#include "session.h"
#include "single_instance_coordinator.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "../common/path_utils.h"

#include <shlobj.h>
#include <algorithm>
#include <memory>

namespace pulse::app {
namespace {

std::wstring KnownDirectory(REFKNOWNFOLDERID id, DWORD flags = KF_FLAG_DONT_VERIFY) {
    PWSTR value = nullptr;
    const auto hr = SHGetKnownFolderPath(id, flags, nullptr, &value);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(value, &CoTaskMemFree);
    return SUCCEEDED(hr) && value ? fs::NormalizePath(value) : std::wstring{};
}

} // namespace

ui::PickerPlaces LoadFilePickerPlaces(const std::function<bool()>& cancelled) {
    ui::PickerPlaces result;
    for (auto& account : BuildOneDriveEntries()) {
        result.onedrive.push_back({std::move(account.label), path::FriendlyPathText(account.path), L"\xE753"});
    }
    if (result.onedrive.empty()) {
        auto directory = KnownDirectory(FOLDERID_OneDrive, 0);
        if (!directory.empty()) result.onedrive.push_back({L"OneDrive", path::FriendlyPathText(directory), L"\xE753"});
    }
    if (cancelled()) return result;

    WindowSessionSnapshot settings;
    LoadSessionPreferences(settings);

    const auto catalog = std::make_shared<model::PlacesCatalog>();
    auto& places = *catalog;
    places.persist = false;
    places.Load();
    result.catalog = catalog;
    auto quick_access = BuildQuickAccessGroup(BuildQuickAccessDefaults(), &places,
        static_cast<uint32_t>(settings.quick_access_hidden));
    for (auto& entry : quick_access.items) {
        if (entry.path.empty() || (fs::IsVirtualPath(entry.path) && entry.path != MakeRecentPath())) continue;
        result.quick_access.push_back({std::move(entry.label), path::FriendlyPathText(entry.path),
            std::move(entry.icon_glyph), entry.icon_color, entry.use_path_icon});
    }
    if (cancelled()) return result;

    std::vector<NetworkLocation> pinned;
    for (const auto& location : places.networks) {
        if (!fs::IsUncPath(location.unc)) continue;
        pinned.push_back({location.name.empty() ? TabTitle(location.unc) : location.name, location.unc});
    }
    const auto net_hood = KnownDirectory(FOLDERID_NetHood);
    auto system = net_hood.empty() ? NetworkLocationScan{} : ReadNetworkLocations(net_hood, cancelled);
    std::erase_if(system.items, [](const auto& location) { return location.shell_link; });
    for (const auto& location : MergeNetworkLocations(pinned, system.items)) {
        result.network.push_back({location.name, path::FriendlyPathText(fs::NormalizePath(location.path)), L"\xE968"});
    }
    return result;
}

ui::PickerPlaces LoadTakeoverPickerPlaces(const std::function<bool()>& cancelled) {
    auto tabs = ReadPickerTabs(FindWindowW(SingleInstanceCoordinator::WindowClassName(), nullptr));
    auto places = LoadFilePickerPlaces(cancelled);
    places.open_tabs = std::move(tabs);
    return places;
}

} // namespace pulse::app
