// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string>

namespace pulse::storage {
// Caller excludes all filename-index writers for the duration of this operation.
bool ApplyUserIndexCheckpoint(std::wstring& error);
bool ApplyUserIndexWithHost(const std::wstring& pipe_name, const std::wstring& mutex_name,
                            std::wstring& error);
}
