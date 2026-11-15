// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "../common/file_identity.h"

namespace pulse::shell {
struct RecycleResult {
    HRESULT error = S_OK;
    bool cancelled = false;
    std::wstring failed_path;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations;
    bool identity_mismatch = false;
};
using RecycleCancelled = std::function<bool()>;
using RecycleProgress = std::function<void(size_t, size_t, const std::wstring&)>;
// STA worker only, after host admission. The shared IFileOperation engine
// requests recycling and retains the Shell permanent-deletion warning.
RecycleResult RecycleAuthorized(const std::vector<std::wstring>& paths,
    const RecycleCancelled& cancelled = {}, const RecycleProgress& progress = {},
    const std::vector<FileIdentity>& expected_identities = {});
}
