#pragma once
#include <string>

namespace pulse::index {

// Runs the per-user UNC indexing mode of Pulse.Index.exe. This must remain a
// separate process from the SYSTEM service so SMB uses the interactive user's
// credentials, but it shares the same executable and release artifact.
int RunNetworkAgent();
int RunNetworkAgentFixture(const std::wstring& tag);

} // namespace pulse::index
