// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "file_operations.h"
#include <algorithm>

namespace pulse::shell {
struct PermanentDeleteResult {
    HRESULT error = S_OK;
    bool cancelled = false;
    std::wstring failed_path;
    std::vector<std::wstring> deleted;
};
inline PermanentDeleteResult DeletePermanentAuthorized(const std::vector<std::wstring>& paths,
    const std::vector<FileIdentity>& expected, const std::function<bool()>& cancelled,
    const std::function<void(size_t, size_t, const std::wstring&)>& progress = {},
    bool elevation_authorized = false, HWND owner = nullptr) {
    PermanentDeleteResult result;
    if (paths.empty() || (!expected.empty() && paths.size() != expected.size())) { result.error = E_INVALIDARG; return result; }
    ipc::ShellOperationRequest request;
    request.kind = ipc::ShellOperationKind::DeletePermanent;
    request.elevation_authorized = elevation_authorized;
    request.owner_hwnd = reinterpret_cast<uintptr_t>(owner);
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!expected.empty() && !expected[i].Valid()) { result.error = E_INVALIDARG; return result; }
        request.items.push_back({paths[i], {}, expected.empty() ? FileIdentity{} : expected[i]});
    }
    const auto completed = ExecuteFileOperation(request, {cancelled, {},
        [&](float, const std::wstring& path, size_t done, size_t total) { if (progress) progress(done, total, path); }});
    result.error = completed.hr; result.cancelled = completed.cancelled; result.failed_path = completed.failed_path;
    for (const auto& item : completed.items) {
        if (FAILED(item.hr) || item.skipped) continue;
        if (std::find(paths.begin(), paths.end(), item.source) != paths.end()) result.deleted.push_back(item.source);
    }
    return result;
}
}
