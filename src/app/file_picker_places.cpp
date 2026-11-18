// SPDX-License-Identifier: Apache-2.0
#include "file_picker_places.h"

#include "app_model.h"
#include "network_locations.h"
#include "session.h"
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
    using I = l10n::StringId;
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
    const auto visible = [&](BuiltinQuickAccess item) {
        return (settings.quick_access_hidden & (1 << static_cast<int>(item))) == 0;
    };
    const auto add_known = [&](REFKNOWNFOLDERID id, I label, const wchar_t* glyph) {
        const auto directory = KnownDirectory(id);
        if (!directory.empty()) result.quick_access.push_back({l10n::Get(label), path::FriendlyPathText(directory), glyph});
    };
    if (visible(BuiltinQuickAccess::Desktop)) add_known(FOLDERID_Desktop, I::Desktop, L"\xE7F4");
    if (visible(BuiltinQuickAccess::Downloads)) add_known(FOLDERID_Downloads, I::Downloads, L"\xE896");
    add_known(FOLDERID_Documents, I::KnownFolderDocuments, L"\xE8A5");
    add_known(FOLDERID_Pictures, I::KnownFolderPictures, L"\xEB9F");
    add_known(FOLDERID_Music, I::KnownFolderMusic, L"\xE8D6");
    add_known(FOLDERID_Videos, I::KnownFolderVideos, L"\xE714");

    model::PlacesCatalog places;
    places.persist = false;
    places.Load();
    for (const auto& pin : places.quick_access_paths) {
        auto directory = path::FriendlyPathText(pin);
        if (std::none_of(result.quick_access.begin(), result.quick_access.end(), [&](const auto& entry) {
            return path::EqualInsensitive(entry.directory, directory);
        })) result.quick_access.push_back({TabTitle(pin), std::move(directory), L"\xE8B7"});
    }
    if (!places.quick_access_order.empty()) {
        std::stable_sort(result.quick_access.begin(), result.quick_access.end(), [&](const auto& a, const auto& b) {
            return places.QuickAccessRank(a.directory) < places.QuickAccessRank(b.directory);
        });
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

} // namespace pulse::app
