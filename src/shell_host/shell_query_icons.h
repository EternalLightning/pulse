// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../ipc/shell_query_protocol.h"

struct IWICBitmapSource;

namespace pulse::shell {

bool ConvertShellBitmapPixels(IWICBitmapSource* source, uint32_t list_id, ipc::ShellIconPixels& pixels);

// Caller owns the icon and initializes COM. This conversion also normalizes
// small legacy artwork embedded in a padded jumbo canvas.
bool ConvertShellIconPixels(HICON icon, uint32_t list_id, ipc::ShellIconPixels& pixels);
void QueryShellIcons(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result);

} // namespace pulse::shell
