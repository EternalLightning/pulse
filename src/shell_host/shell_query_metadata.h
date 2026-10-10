#pragma once
#include "../ipc/shell_query_protocol.h"

namespace pulse::shell {

// Native Shell queries for the STA query host. Never call on the UI thread.
void QueryShellName(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result);
void QueryShellShortcut(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result);
void QueryShellNamespace(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result);
void InitializePreviewItem(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result);

} // namespace pulse::shell
