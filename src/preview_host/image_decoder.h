// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {
// File frames read their EXIF orientation; embedded bytes use the supplied main-frame orientation.
bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                 std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
                 UINT& source_width, UINT& source_height,
                 const std::vector<uint8_t>* embedded = nullptr, uint32_t orientation = 1);
}
