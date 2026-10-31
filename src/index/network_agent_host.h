// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string>

namespace pulse::index {

// Runs the per-user UNC indexing mode of Pulse.Index.exe. This must remain a
// separate process from the SYSTEM service so SMB uses the interactive user's
// credentials, but it shares the same executable and release artifact.
int RunNetworkAgent();
int RunNetworkAgentFixture(const std::wstring& tag);

} // namespace pulse::index
