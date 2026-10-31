// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <string>
#include <windows.h>
#include "content_index.h"

namespace pulse::index {

int RunContentAgent(const std::wstring& token);
int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, bool read_only = false);
int RunPersistentContentAgent(const std::wstring& token, DWORD parent_pid, ContentAgentMode mode,
                              const std::wstring& database_path = {});

} // namespace pulse::index
