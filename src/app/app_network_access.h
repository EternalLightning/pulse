#pragma once
#include "network_connect.h"
#include "network_locations.h"
#include <memory>
#include <unordered_map>

namespace pulse { struct AppState; }
namespace pulse::app {
struct Tab;
struct NetworkAccessState {
    const uint64_t epoch = NextNetworkIdentity();
    std::shared_ptr<NetworkConnectMailbox> mailbox = std::make_shared<NetworkConnectMailbox>();
    struct Pending {
        NetworkRequestIdentity identity;
        const Tab* tab = nullptr;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    std::unordered_map<uint64_t, Pending> pending;
    std::unordered_map<uint64_t, uint64_t> foreground_listings;
    std::mutex discovery_mutex;
    NetworkDiscoveryState discovery = NetworkDiscoveryState::Unknown;
    bool discovery_pending = false;
    bool discovery_ready = false;
    bool closed = false;
    ULONGLONG discovery_requested = 0;
};
}
namespace pulse {
void RequestNetworkAuthentication(AppState& s, app::Tab& tab, DWORD error);
void MarkNetworkForegroundListing(AppState& s, const app::Tab& tab);
bool TakeNetworkForegroundListing(AppState& s, const app::Tab& tab, uint64_t generation);
bool NetworkForegroundRequestPending(const AppState& s, const app::Tab& tab);
void CancelNetworkAuthenticationForTab(AppState& s, const app::Tab& tab);
void CancelNetworkAuthentication(AppState& s);
void PumpNetworkAccess(AppState& s);
void CancelNetworkAccess(AppState& s);
void RequestNetworkDiscovery(AppState& s);
bool NetworkDiscoveryDisabled(const AppState& s);
void OpenNetworkSettings(HWND owner);
}
