// SPDX-License-Identifier: Apache-2.0
#include "recycle_fileop.h"
#include "file_operations.h"
#include <algorithm>

namespace pulse::shell {
RecycleResult RecycleAuthorized(const std::vector<std::wstring>& paths,
    const RecycleCancelled& cancelled, const RecycleProgress& progress,
    const std::vector<FileIdentity>& expected_identities) {
    RecycleResult result;
    if (paths.empty() || (!expected_identities.empty() && paths.size() != expected_identities.size())) {
        result.error = E_INVALIDARG; return result;
    }
    ipc::ShellOperationRequest request;
    request.kind = ipc::ShellOperationKind::Recycle;
    for (size_t i = 0; i < paths.size(); ++i)
        request.items.push_back({paths[i], {}, expected_identities.empty() ? FileIdentity{} : expected_identities[i]});
    const auto completed = ExecuteFileOperation(request, {cancelled, {},
        [&](float, const std::wstring& path, size_t done, size_t total) { if (progress) progress(done, total, path); }});
    result.error = completed.hr; result.cancelled = completed.cancelled; result.failed_path = completed.failed_path;
    result.identity_mismatch = completed.hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    for (const auto& item : completed.items) {
        if (FAILED(item.hr) || item.skipped || item.destination.empty()) continue;
        if (std::find(paths.begin(), paths.end(), item.source) == paths.end()) continue;
        result.sources.push_back(item.source); result.destinations.push_back(item.destination);
    }
    return result;
}
}
