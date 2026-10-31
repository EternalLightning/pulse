// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string>

namespace pulse::index {
// Called by the elevated configuration helper, never by the UI thread.
int ConfigureServiceIndexPath(const std::wstring& path);
}
