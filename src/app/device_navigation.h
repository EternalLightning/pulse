// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "device_registry.h"
#include "device_messages.h"

namespace pulse { struct AppState; }
namespace pulse::app {
struct Tab;
struct DeviceOperationResult {
    HRESULT error = S_OK;
    bool cancelled = false;
};
bool IsDeviceTab(const Tab* tab);
bool HasDeviceSelection(const Tab& tab);
std::vector<std::wstring> DeviceSelectedIds(const Tab& tab);
std::wstring DeviceTitle(const std::wstring& path);
std::wstring DeviceParent(const std::wstring& path);
void QueueDeviceOperation(AppState& state, std::wstring verb,
    std::vector<std::wstring> ids, std::wstring name = {});
void ApplyDeviceOperationResult(AppState& state, const DeviceOperationResult& result);
void SetDeviceError(Tab& tab, fs::DeviceError error);
void DevicesChanged(AppState& state);
}
