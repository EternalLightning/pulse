// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace pulse::bench {

struct BitmapArtifactResult {
    std::filesystem::path path;
    std::string error;
    bool saved = false;
};

inline BitmapArtifactResult SaveBitmapArtifact(const std::vector<std::uint32_t>& pixels,
                                               int width, int height,
                                               const std::filesystem::path& path) {
    BitmapArtifactResult result;
    result.path = path;
    const auto fail = [&](const char* operation, const std::error_code& error) {
        result.error = std::string(operation) + ": " + error.message();
        return result;
    };
    const auto io_error = [] {
        return errno ? std::error_code(errno, std::generic_category()) :
            std::make_error_code(std::errc::io_error);
    };
    std::error_code error;
    result.path = std::filesystem::absolute(path, error);
    if (error) return fail("resolve path", error);
    const uint64_t pixel_count = width > 0 && height > 0 ?
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) : 0;
    constexpr size_t header_bytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    if (!pixel_count || pixel_count != pixels.size() ||
        pixel_count > ((std::numeric_limits<DWORD>::max)() - header_bytes) / sizeof(pixels[0]))
        return fail("validate pixels", std::make_error_code(std::errc::invalid_argument));
    const auto pixel_bytes = static_cast<DWORD>(pixel_count * sizeof(pixels[0]));
    std::filesystem::create_directories(result.path.parent_path(), error);
    if (error) return fail("create artifact directory", error);
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = static_cast<DWORD>(header_bytes);
    header.bfSize = header.bfOffBits + pixel_bytes;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    errno = 0;
    std::ofstream output(result.path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) return fail("open artifact", io_error());
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixel_bytes));
    output.flush();
    if (!output) return fail("write artifact", io_error());
    output.close();
    if (!output) return fail("close artifact", io_error());
    const auto bytes = std::filesystem::file_size(result.path, error);
    if (error) return fail("verify artifact length", error);
    if (bytes != header.bfSize) return fail("verify artifact length", std::make_error_code(std::errc::io_error));
    result.saved = true;
    return result;
}

inline std::string BitmapArtifactPath(const BitmapArtifactResult& result) {
    const auto utf8 = result.path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace pulse::bench
