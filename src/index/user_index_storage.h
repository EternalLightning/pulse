#pragma once
#include <string>

namespace pulse::storage {
// Caller excludes all filename-index writers for the duration of this operation.
bool ApplyUserIndexCheckpoint(std::wstring& error);
bool ApplyUserIndexWithHost(const std::wstring& pipe_name, const std::wstring& mutex_name,
                            std::wstring& error);
}
