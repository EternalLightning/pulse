#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {
bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                 std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
                 UINT& source_width, UINT& source_height,
                 const std::vector<uint8_t>* embedded = nullptr, uint32_t orientation = 1);
}
