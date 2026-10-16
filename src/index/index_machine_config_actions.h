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
