#pragma once
#include "../ipc/shell_launch_protocol.h"
#include <functional>

namespace pulse::shell {
HRESULT PickShellPath(const ipc::ShellLaunchRequest& request, HWND owner, std::wstring& selected);
HRESULT RunElevated(const ipc::ShellLaunchRequest& request, std::wstring& exit_code,
                    const std::function<bool()>& heartbeat);
}
