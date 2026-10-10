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
