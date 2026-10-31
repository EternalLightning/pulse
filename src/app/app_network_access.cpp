// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_network_access.h"
#include "app_internal.h"
#include "server_address.h"
#include "../common/localization.h"
#include <winnetwk.h>
#include <shellapi.h>
#include <thread>

namespace pulse {
namespace {
std::shared_ptr<app::NetworkAccessState> State(AppState& s) {
    if (!s.network_access) s.network_access = std::make_shared<app::NetworkAccessState>();
    return s.network_access;
}
app::NetworkRequestIdentity Identity(const app::NetworkAccessState& state, const app::Tab& tab) {
    return {state.epoch, tab.network_identity, tab.view_generation, tab.applied_generation, tab.current_path};
}
}
void MarkNetworkForegroundListing(AppState& s, const app::Tab& tab) {
    if (fs::IsUncPath(tab.current_path) && tab.pending_generation)
        State(s)->foreground_listings[tab.network_identity] = tab.pending_generation;
}
bool TakeNetworkForegroundListing(AppState& s, const app::Tab& tab, uint64_t generation) {
    if (!s.network_access) return false;
    const auto found = s.network_access->foreground_listings.find(tab.network_identity);
    if (found == s.network_access->foreground_listings.end() || found->second != generation) return false;
    s.network_access->foreground_listings.erase(found);
    return true;
}
bool NetworkForegroundRequestPending(const AppState& s, const app::Tab& tab) {
    if (!s.network_access) return false;
    return s.network_access->pending.contains(tab.network_identity) ||
        s.network_access->foreground_listings.contains(tab.network_identity);
}
void RequestNetworkAuthentication(AppState& s, app::Tab& tab, DWORD error) {
    if (s.exit_requested || s.core.exiting || ActiveTab(s) != &tab || !IsWindowVisible(s.hwnd)) return;
    if (!app::IsNetworkAuthenticationError(error) || !fs::IsUncPath(tab.current_path) || tab.loading) return;
    auto share = app::UncShareRoot(tab.current_path);
    if (share.empty()) {
        const auto address = app::ParseServerAddress(tab.current_path);
        if (address.kind == app::ServerAddressKind::Server) share = address.unc + L"\\IPC$";
    }
    if (share.empty()) return;
    auto state = State(s);
    const auto identity = Identity(*state, tab);
    if (state->pending.contains(tab.network_identity) || !app::BeginNetworkShareRequest(share)) return;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    state->pending.emplace(tab.network_identity, app::NetworkAccessState::Pending{identity, &tab, cancelled});
    const auto mailbox = state->mailbox;
    const HWND owner = s.hwnd;
    try {
        // Windows owns the credential UI and secret buffers. This synchronous
        // provider call cannot be interrupted reliably; shutdown only closes
        // the result mailbox and never waits on a provider or credential dialog.
        std::thread([mailbox, identity, share, owner, cancelled] {
            DWORD code = ERROR_CANCELLED;
            {
                std::lock_guard lock(mailbox->mutex);
                if (mailbox->closed || cancelled->load()) { app::EndNetworkShareRequest(share); return; }
            }
            NETRESOURCEW resource{};
            resource.dwType = share.ends_with(L"\\IPC$") ? RESOURCETYPE_ANY : RESOURCETYPE_DISK;
            resource.lpRemoteName = const_cast<wchar_t*>(share.c_str());
            code = WNetAddConnection3W(IsWindow(owner) ? owner : nullptr, &resource, nullptr, nullptr, CONNECT_INTERACTIVE);
            app::EndNetworkShareRequest(share);
            if (!cancelled->load()) mailbox->Publish({identity, code});
        }).detach();
    } catch (...) {
        app::EndNetworkShareRequest(share);
        state->pending.erase(tab.network_identity);
        return;
    }
    tab.SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::NetworkSignInTitle));
    tab.banner_message = l10n::Get(l10n::StringId::NetworkSignInHint);
}
void CancelNetworkAuthenticationForTab(AppState& s, const app::Tab& tab) {
    if (s.network_access) {
        const auto found = s.network_access->pending.find(tab.network_identity);
        if (found != s.network_access->pending.end()) found->second.cancelled->store(true);
        s.network_access->pending.erase(tab.network_identity);
        s.network_access->foreground_listings.erase(tab.network_identity);
    }
}
void CancelNetworkAuthentication(AppState& s) {
    if (s.network_access) {
        for (const auto& [id, pending] : s.network_access->pending) {
            (void)id;
            pending.cancelled->store(true);
        }
        s.network_access->pending.clear();
        s.network_access->foreground_listings.clear();
    }
}
void PumpNetworkAccess(AppState& s) {
    const auto state = s.network_access;
    if (!state) return;
    for (const auto& result : state->mailbox->Take()) {
        const auto found = state->pending.find(result.identity.tab);
        if (found == state->pending.end() || found->second.identity != result.identity) continue;
        const auto pending = found->second;
        state->pending.erase(found);
        ForEachPane(s, [&](app::Pane& pane) {
            auto* tab = pane.ActiveTab();
            if (!tab || !tab->net_readonly || !app::NetworkResultMatches(result, pending.identity,
                    Identity(*state, *tab), pending.tab, tab, tab->loading)) return;
            if (result.error == NO_ERROR) StartLoadingPath(s, *tab, tab->current_path, PathLoadReason::NetworkRetry);
            else {
                tab->pending_network_pin.clear();
                tab->SetBannerTitle(app::BannerType::Other, l10n::Get(l10n::StringId::CannotOpen));
                tab->banner_message = app::NetworkConnectErrorText(result.error);
                if (tab->snapshot) tab->banner_message += L" · " + l10n::Get(l10n::StringId::CachedReadOnly);
            }
        });
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    {
        std::lock_guard lock(state->discovery_mutex);
        if (state->discovery_ready) {
            state->discovery_ready = false;
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
    }
}
void CancelNetworkAccess(AppState& s) {
    if (!s.network_access) return;
    s.network_access->mailbox->Close();
    CancelNetworkAuthentication(s);
    std::lock_guard lock(s.network_access->discovery_mutex);
    s.network_access->closed = true;
}
void RequestNetworkDiscovery(AppState& s) {
    const auto state = State(s);
    {
        std::lock_guard lock(state->discovery_mutex);
        const auto now = GetTickCount64();
        if (state->closed || state->discovery_pending ||
            (state->discovery_requested && now - state->discovery_requested < 5000)) return;
        state->discovery_pending = true;
        state->discovery_requested = now;
    }
    try {
        std::thread([state] {
            const auto result = app::QueryNetworkDiscoveryState();
            std::lock_guard lock(state->discovery_mutex);
            state->discovery_pending = false;
            if (!state->closed) { state->discovery = result; state->discovery_ready = true; }
        }).detach();
    } catch (...) {
        std::lock_guard lock(state->discovery_mutex);
        state->discovery_pending = false;
        state->discovery = app::NetworkDiscoveryState::Unknown;
    }
}
bool NetworkDiscoveryDisabled(const AppState& s) {
    if (!s.network_access) return false;
    std::lock_guard lock(s.network_access->discovery_mutex);
    return !s.network_access->closed && s.network_access->discovery == app::NetworkDiscoveryState::Disabled;
}
void OpenNetworkSettings(HWND owner) {
    auto opened = ShellExecuteW(owner, L"open", L"ms-settings:network-advancedsettings", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
        ShellExecuteW(owner, L"open", L"control.exe", L"/name Microsoft.NetworkAndSharingCenter", nullptr, SW_SHOWNORMAL);
}
}
