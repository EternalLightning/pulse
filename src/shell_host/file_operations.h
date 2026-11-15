// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ipc/file_operation_protocol.h"
#include <functional>

namespace pulse::shell {
struct FileOperationCallbacks {
    std::function<bool()> cancelled;
    std::function<bool()> paused;
    std::function<void(float, const std::wstring&, size_t, size_t)> progress;
    std::function<void(uint64_t, uint64_t)> bytes_progress;
};
// The caller owns COM initialization and must run on an STA.
ipc::ShellOperationResult ExecuteFileOperation(const ipc::ShellOperationRequest& request,
    const FileOperationCallbacks& callbacks = {});
}
