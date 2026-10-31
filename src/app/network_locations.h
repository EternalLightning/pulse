// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
namespace pulse::app {
enum class NetworkDiscoveryState { Unknown, Disabled, Enabled };
NetworkDiscoveryState SummarizeNetworkDiscoveryProfiles(const std::vector<NetworkDiscoveryState>& profiles);
NetworkDiscoveryState QueryNetworkDiscoveryState();

inline std::wstring NetworkLocationKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() >= 8 && CompareStringOrdinal(path.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL)
        path = L"\\\\" + path.substr(8);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}
inline bool SameNetworkLocation(const std::wstring& a, const std::wstring& b) {
    const auto left = NetworkLocationKey(a), right = NetworkLocationKey(b);
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}
struct NetworkLocation { std::wstring name, path; bool shell_link = false; };
struct NetworkLocationScan {
    std::vector<NetworkLocation> items;
    NetworkDiscoveryState discovery = NetworkDiscoveryState::Unknown;
    DWORD error = 0;
    bool cancelled = false;
};
// Local NetHood shortcut metadata only: never resolves or probes targets.
NetworkLocationScan ReadNetworkLocations(const std::wstring& root = {},
    const std::function<bool()>& cancel = {});
std::vector<NetworkLocation> MergeNetworkLocations(const std::vector<NetworkLocation>& pinned,
    const std::vector<NetworkLocation>& system);
}
