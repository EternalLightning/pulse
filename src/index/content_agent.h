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
