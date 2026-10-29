#include "app_network_locations.h"
#include "app_internal.h"
#include "lan_discovery.h"
#include "network_locations.h"
#include "server_address.h"
#include "../common/localization.h"
#include "../ui/connect_server_dialog.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <shlwapi.h>

namespace pulse {
struct NetworkLocationLoad {
    std::atomic_bool cancelled{false};
    std::mutex mutex;
    app::NetworkLocationScan result;
    app::LanDiscoveryResult lan_result;
    bool ready = false;
    bool pending = false; // UI thread only
    DWORD error = 0;
    app::NetworkDiscoveryState discovery = app::NetworkDiscoveryState::Unknown;
    std::vector<app::NetworkLocation> items;
    app::LanDiscoveryResult lan;
    uint64_t recent_revision = 0;
};
namespace {
std::shared_ptr<NetworkLocationLoad> State(AppState& s) {
    if (!s.network_locations) s.network_locations = std::make_shared<NetworkLocationLoad>();
    if (!s.core.network_recent)
        s.core.network_recent = std::make_shared<app::NetworkRecentStore>(!s.isolatedTest && !s.shot.active && !s.menushot);
    return s.network_locations;
}
std::vector<app::NetworkLocation> Locations(const AppState& s) {
    std::vector<app::NetworkLocation> pins;
    for (const auto& pin : s.places.networks)
        pins.push_back({pin.name.empty() ? pin.unc : pin.name, pin.unc, false});
    return app::MergeNetworkLocations(pins, s.network_locations
        ? s.network_locations->items : std::vector<app::NetworkLocation>{});
}
void RefreshViews(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (tab && tab->current_path == kNetworkLocationsPath) FillNetworkLocationsView(s, *tab);
    });
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}
void RequestNetworkLocations(AppState& s) {
    if (s.exit_requested || s.core.exiting) return;
    const auto load = State(s);
    if (load->pending || load->cancelled) return;
    load->pending = true;
    std::wstring fixture;
    if (s.isolatedTest) {
        wchar_t path[32768]{};
        const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_NETWORK_LOCATIONS", path, ARRAYSIZE(path));
        if (size && size < ARRAYSIZE(path)) fixture.assign(path, size);
    }
    const bool isolated = s.isolatedTest || s.shot.active || s.menushot;
    try {
        std::thread([load, fixture = std::move(fixture), isolated] {
            const auto cancel = [load] { return load->cancelled.load(); };
            auto locations = isolated && fixture.empty() ? app::NetworkLocationScan{} : app::ReadNetworkLocations(fixture, cancel);
            auto lan = isolated || cancel() ? app::LanDiscoveryResult{} : app::RunLanDiscovery(cancel);
            std::lock_guard lock(load->mutex);
            if (cancel()) return;
            load->result = std::move(locations);
            load->lan_result = std::move(lan);
            load->ready = true;
        }).detach();
    } catch (...) {
        load->pending = false;
        load->error = ERROR_NOT_ENOUGH_MEMORY;
    }
}
void FillNetworkLocationsView(AppState& s, app::Tab& tab) {
    const auto load = State(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    const auto add = [&](std::wstring name, std::wstring path, std::wstring detail, bool directory = true) {
        if (path.empty()) return;
        if (std::any_of(entries->begin(), entries->end(), [&](const auto& entry) {
            return app::SameNetworkLocation(entry.full_path, path);
        })) return;
        fs::DirEntry entry;
        entry.name = std::move(name);
        entry.full_path = std::move(path);
        entry.is_dir = directory;
        entry.attrs = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entry.change_type_text = std::move(detail);
        entries->push_back(std::move(entry));
    };
    add(l10n::Get(l10n::StringId::ConnectServerTitle), kConnectServerPath,
        l10n::Get(l10n::StringId::ConnectServerHint));
    for (const auto& item : Locations(s))
        add(item.name, item.path, item.shell_link ? l10n::Get(l10n::StringId::OpensInWindows) :
            app::NetworkLocationKey(item.path), !item.shell_link);
    for (const auto& device : load->lan.devices)
        add(device.name, app::LanDeviceUnc(device), l10n::Get(l10n::StringId::NetworkLanDevices) + L" · " +
            (device.model.empty() ? device.host : device.model));
    const auto recent_list = s.core.network_recent->Snapshot();
    for (const auto& recent : recent_list.Items())
        add(app::NetworkLocationKey(recent.path), recent.path, l10n::Get(l10n::StringId::NetworkRecentVisits));
    std::vector<std::wstring> selected;
    for (int index : tab.SelectedIndices()) if (index >= 0 && static_cast<size_t>(index) < tab.EntryCount())
        selected.push_back(tab.EntryAt(static_cast<size_t>(index)).full_path);
    const std::wstring focus = tab.selected_index >= 0 && static_cast<size_t>(tab.selected_index) < tab.EntryCount()
        ? tab.EntryAt(static_cast<size_t>(tab.selected_index)).full_path : std::wstring{};
    tab.virtual_title = l10n::Get(l10n::StringId::SidebarNetworkLocations);
    tab.SetSnapshot(std::move(entries));
    tab.ClearSelection();
    for (size_t i = 0; i < tab.EntryCount(); ++i) {
        const auto& path = tab.EntryAt(i).full_path;
        if (std::find(selected.begin(), selected.end(), path) != selected.end()) tab.ToggleSelect(static_cast<int>(i));
        if (path == focus) tab.selected_index = tab.selection_anchor = static_cast<int>(i);
    }
    // Keep the connection entry available while a background browse is running.
    tab.loading = false;
    tab.ClearBanner();
    if (load->pending) {
        tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::NetworkDiscovering));
        tab.banner_message = l10n::Get(l10n::StringId::ConnectServerHint);
    } else if (load->error) {
        tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::CouldNotReadSystemNetworkLocations));
        tab.banner_message = app::NetworkConnectErrorText(load->error);
    } else if (load->lan.public_network || load->lan.offline) {
        tab.SetBannerTitle(app::BannerType::Other, l10n::Get(load->lan.public_network
            ? l10n::StringId::NetworkPublicDiscoverySkipped : l10n::StringId::NetworkOffline));
        tab.banner_message = l10n::Get(l10n::StringId::ConnectServerHint);
    } else if (load->discovery == app::NetworkDiscoveryState::Disabled) {
        tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::NetworkDiscoveryOffTitle));
        tab.banner_message = l10n::Get(l10n::StringId::NetworkDiscoveryOffDesc);
    } else if (load->lan.devices.empty()) {
        tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::NetworkNoDevices));
        tab.banner_message = l10n::Get(l10n::StringId::ConnectServerHint);
    }
}
void PumpNetworkLocations(AppState& s) {
    const auto load = s.network_locations;
    if (!load || load->cancelled) return;
    bool changed = false;
    {
        std::lock_guard lock(load->mutex);
        if (load->ready) {
            load->ready = false;
            load->pending = false;
            load->error = load->result.error;
            if (!load->result.cancelled && !load->result.error) load->items = std::move(load->result.items);
            load->discovery = load->result.discovery;
            if (!load->lan_result.cancelled) load->lan = std::move(load->lan_result);
            changed = true;
        }
    }
    uint64_t revision = 0;
    if (s.core.network_recent) s.core.network_recent->Snapshot(&revision);
    if (revision != load->recent_revision) { load->recent_revision = revision; changed = true; }
    if (!changed) return;
    s.sidebar.system_networks.clear();
    for (const auto& item : load->items) {
        app::SidebarEntry entry;
        entry.label = item.name;
        entry.path = item.path;
        entry.detail = item.shell_link ? l10n::Get(l10n::StringId::OpensInWindows) : item.path;
        s.sidebar.system_networks.push_back(std::move(entry));
    }
    for (const auto& device : load->lan.devices) {
        const auto path = app::LanDeviceUnc(device);
        if (path.empty()) continue;
        app::SidebarEntry entry;
        entry.label = device.name;
        entry.path = path;
        entry.detail = device.model.empty() ? path : device.model;
        s.sidebar.system_networks.push_back(std::move(entry));
    }
    RefreshViews(s);
}
void CancelNetworkLocations(AppState& s) {
    if (s.network_locations) s.network_locations->cancelled = true;
}
bool OpenSystemNetworkShortcut(AppState& s, const std::wstring& path) {
    if (!s.network_locations) return false;
    for (const auto& item : s.network_locations->items) {
        if (item.shell_link && app::SameNetworkLocation(item.path, path)) {
            s.ops.OpenWith(path);
            return true;
        }
    }
    return false;
}
void ShowConnectServer(AppState& s) {
    if (s.exit_requested || s.core.exiting || !ActiveTab(s)) return;
    const auto load = State(s);
    ui::ConnectServerDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::ConnectServerTitle);
    spec.label = l10n::Get(l10n::StringId::ConnectServerAddress);
    spec.confirm_text = l10n::Get(l10n::StringId::ConnectServerConnect);
    spec.preview = [](const std::wstring& text) {
        const auto address = app::ParseServerAddress(text, true);
        const bool valid = address.kind == app::ServerAddressKind::Server || address.kind == app::ServerAddressKind::Share;
        return ui::ConnectServerPreview{valid, text.empty() ? std::wstring{} : valid ? address.unc :
            l10n::Get(l10n::StringId::ConnectServerInvalidAddress)};
    };
    const auto locations = Locations(s);
    const auto recent = s.core.network_recent->Snapshot();
    spec.suggestions = [locations, recent, devices = load->lan.devices](const std::wstring& text) {
        std::vector<ui::ConnectServerSuggestion> out;
        const auto add = [&](const std::wstring& path, const std::wstring& hint) {
            const auto parsed = app::ParseServerAddress(path);
            if (parsed.kind != app::ServerAddressKind::Server && parsed.kind != app::ServerAddressKind::Share) return;
            if (!text.empty() && !StrStrIW(path.c_str(), text.c_str())) return;
            if (std::none_of(out.begin(), out.end(), [&](const auto& item) { return app::SameNetworkLocation(item.path, path); }))
                out.push_back({path, hint});
        };
        for (const auto& item : locations) if (!item.shell_link) add(item.path, item.name);
        for (const auto& device : devices) add(app::LanDeviceUnc(device), device.name);
        for (const auto& item : recent.Items()) add(item.path, l10n::Get(l10n::StringId::NetworkRecentVisits));
        return out;
    };
    const auto result = ui::ShowConnectServerDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    if (!result.accepted || !IsWindow(s.hwnd) || s.exit_requested || s.core.exiting) return;
    const auto address = app::ParseServerAddress(result.text, true);
    if (address.kind != app::ServerAddressKind::Server && address.kind != app::ServerAddressKind::Share) return;
    NavigateTo(s, address.unc);
    if (auto* tab = ActiveTab(s); result.pin && tab && app::SameNetworkLocation(tab->current_path, address.unc))
        tab->pending_network_pin = address.unc;
}
void RecordNetworkLocationOpened(AppState& s, app::Tab& tab) {
    if (!fs::IsUncPath(tab.current_path) || tab.net_readonly) return;
    State(s);
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    s.core.network_recent->Touch(tab.current_path, ticks.QuadPart);
    if (!tab.pending_network_pin.empty()) {
        if (app::SameNetworkLocation(tab.pending_network_pin, tab.current_path)) {
            s.places.PinNetwork(app::NetworkRecentRoot(tab.current_path), L"");
            s.core.windows.ForEach([](HWND, AppState& window) { RefreshViews(window); });
        }
        tab.pending_network_pin.clear();
    }
}
}
