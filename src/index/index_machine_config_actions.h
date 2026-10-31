// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_config.h"
#include <windows.h>

namespace pulse::index {
template<class Load, class Save>
DWORD PrepareMachineConfiguration(Load load, Save save) {
    IndexConfig config;
    if (!load(config)) return ERROR_INVALID_DATA;
    return save(config) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
template<class Load>
std::wstring LoadMachineIndexDirectory(Load load) {
    IndexConfig config;
    return load(config) ? config.index_path : std::wstring{};
}
}
