// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#pragma once

#include "file_dialog_contract.h"

#include <functional>
#include <shobjidl.h>

namespace pulse::shell_bridge {

struct FileDialogBridgeServices {
    std::wstring executable;
    std::function<bool()> enabled;
    std::function<ipc::picker::Message(const ipc::picker::Message&, HWND, HANDLE)> run_bridge;
};

HRESULT WrapNativeFileDialog(DialogFactory factory, IUnknown* native,
    const FileDialogBridgeServices& services, REFIID iid, void** output) noexcept;

} // namespace pulse::shell_bridge
