// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <memory>
#include <string>

namespace pulse {
struct AppState;
struct NetworkLocationLoad;
namespace app { struct Tab; }
inline constexpr wchar_t kNetworkLocationsPath[] = L"pulse:networks";
inline constexpr wchar_t kConnectServerPath[] = L"pulse:connect-server";
void RequestNetworkLocations(AppState& state);
void PumpNetworkLocations(AppState& state);
void CancelNetworkLocations(AppState& state);
void FillNetworkLocationsView(AppState& state, app::Tab& tab);
void ShowConnectServer(AppState& state);
bool OpenSystemNetworkShortcut(AppState& state, const std::wstring& path);
void RecordNetworkLocationOpened(AppState& state, app::Tab& tab);
}
