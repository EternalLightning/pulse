#pragma once

#include <string>
#include <windows.h>

namespace pulse::ops {

// Runs on the open worker's COM apartment. Returns false when the path is not
// an image or Shell folder activation is unavailable; the caller uses ShellExecute.
bool TryOpenImageWithNeighbors(HWND owner, const std::wstring& path) noexcept;

} // namespace pulse::ops
