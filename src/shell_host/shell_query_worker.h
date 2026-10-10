#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::shell {
int RunShellQueryWorker(DWORD parent_pid, uint32_t session);
}
