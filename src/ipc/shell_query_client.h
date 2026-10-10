#pragma once

#include "shell_query_protocol.h"
#include <functional>

namespace pulse::ipc {

// Blocking, bounded Shell IPC. Call only from a background worker.
// Cancellation kills only the leased query process; subsequent calls recover.
// Preview initialization is never replayed after a lost response.
bool QueryShell(const ShellQueryRequest& request, ShellQueryResult& result,
                const std::function<bool()>& cancelled = {}, DWORD timeout_ms = 4000);

} // namespace pulse::ipc
