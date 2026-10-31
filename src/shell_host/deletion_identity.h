// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <string_view>
#include "../common/file_identity.h"

namespace pulse::shell {
bool IsSameDeletionItem(std::wstring_view requested, std::wstring_view resolved,
    decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx);

}
