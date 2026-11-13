// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../common/path_utils.h"
#include <shlwapi.h>

namespace pulse::shell {

inline std::wstring ContextInvokeDirectory(const std::wstring& path, bool background) {
    auto directory = pulse::path::StripExtendedPathPrefix(path);
    if (directory.empty() || directory.starts_with(L"::") || directory.starts_with(L"shell:")) return {};
    if (background || PathIsRootW(directory.c_str())) return directory;
    const auto slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    directory.resize(slash == 2 && directory[1] == L':' ? 3 : slash);
    return directory;
}

} // namespace pulse::shell
