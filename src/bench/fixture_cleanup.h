// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "shell_host/file_operations.h"
#include <objbase.h>
#include <filesystem>

namespace pulse::bench {
inline void RemoveFixtureTree(const std::filesystem::path& root, std::error_code& error) {
    // Callers verify exclusive ownership; use the same Shell backend as actual directory deletion.
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { error.assign(static_cast<int>(initialized), std::system_category()); return; }
    ipc::ShellOperationRequest request;
    request.kind = ipc::ShellOperationKind::DeletePermanent;
    request.items.push_back({root.wstring(), {}, {}, {}});
    const auto result = shell::ExecuteFileOperation(request);
    if (FAILED(result.hr)) error.assign(static_cast<int>(result.hr), std::system_category());
    CoUninitialize();
}
}
