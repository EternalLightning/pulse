// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::text {

bool IsOfflinePlaceholder(DWORD attributes) noexcept;
bool IsKnownTextExtension(std::wstring_view extension) noexcept;
bool LooksBinary(const std::vector<uint8_t>& bytes) noexcept;
enum class Encoding : uint32_t { Auto = 0, Utf8 = 1, System = 2, Gb18030 = 3 };

bool Decode(const std::vector<uint8_t>& bytes, std::wstring& output, Encoding encoding = Encoding::Auto);
bool ReadFile(const std::wstring& path, uint64_t maximum_bytes, std::wstring& output,
              uint64_t& bytes_read, DWORD* error = nullptr, Encoding encoding = Encoding::Auto);

} // namespace pulse::text
