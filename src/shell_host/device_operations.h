#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse::shell {
struct DeviceOperation {
    HWND owner = nullptr;
    std::wstring verb;
    std::wstring name;
    std::vector<std::wstring> ids;
};
// Called on the host STA; clipboard objects stay on that apartment.
HRESULT ExecuteDeviceOperation(const DeviceOperation& request);
}
