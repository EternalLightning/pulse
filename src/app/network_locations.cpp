// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "network_locations.h"
#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <wrl/client.h>

namespace pulse::app {
NetworkDiscoveryState SummarizeNetworkDiscoveryProfiles(const std::vector<NetworkDiscoveryState>& profiles) {
    if (profiles.empty()) return NetworkDiscoveryState::Unknown;
    const auto first = profiles.front();
    for (const auto state : profiles) if (state != first) return NetworkDiscoveryState::Unknown;
    return first;
}
NetworkDiscoveryState QueryNetworkDiscoveryState() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return NetworkDiscoveryState::Unknown;
    NetworkDiscoveryState result = NetworkDiscoveryState::Unknown;
    {
        Microsoft::WRL::ComPtr<INetFwPolicy2> firewall;
        if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&firewall)))) {
            long active = 0;
            BSTR group = SysAllocString(L"@FirewallAPI.dll,-32752");
            if (group && SUCCEEDED(firewall->get_CurrentProfileTypes(&active))) {
                std::vector<NetworkDiscoveryState> profiles;
                for (const auto profile : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
                    if (!(active & static_cast<long>(profile))) continue;
                    VARIANT_BOOL enabled = VARIANT_FALSE, discovery = VARIANT_FALSE;
                    profiles.push_back(firewall->get_FirewallEnabled(profile, &enabled) == S_OK && enabled == VARIANT_TRUE &&
                        firewall->IsRuleGroupEnabled(static_cast<long>(profile), group, &discovery) == S_OK
                        ? (discovery == VARIANT_TRUE ? NetworkDiscoveryState::Enabled : NetworkDiscoveryState::Disabled)
                        : NetworkDiscoveryState::Unknown);
                }
                result = SummarizeNetworkDiscoveryProfiles(profiles);
            }
            SysFreeString(group);
        }
    }
    CoUninitialize();
    return result;
}
}
