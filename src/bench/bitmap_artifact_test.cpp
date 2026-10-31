// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "bitmap_artifact.h"
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <iterator>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::bench;
    if (argc != 2) return 2;
    GUID guid{};
    wchar_t suffix[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, suffix, ARRAYSIZE(suffix))) return 2;
    const auto root = std::filesystem::absolute(std::filesystem::path(argv[1]) /
        (std::wstring(L"bitmap-artifact-") + suffix));
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { if (!path.empty()) { std::error_code error; std::filesystem::remove_all(path, error); } }
    } cleanup{root};
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const std::vector<std::uint32_t> pixels{0xff112233, 0xff445566, 0xff778899, 0xffaabbcc};
    const auto target = root / L"fresh" / L"nested" / L"sharp-failure.bmp";
    const auto saved = SaveBitmapArtifact(pixels, 2, 2, target);
    check(saved.saved && saved.error.empty() && saved.path == target && saved.path.is_absolute() &&
        std::filesystem::is_regular_file(saved.path), "save creates missing artifact directories and returns the actual absolute path");
    std::ifstream image(saved.path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(image)), std::istreambuf_iterator<char>());
    const size_t header_bytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    check(bytes.size() == header_bytes + pixels.size() * sizeof(pixels[0]) &&
        std::memcmp(bytes.data() + header_bytes, pixels.data(), pixels.size() * sizeof(pixels[0])) == 0,
        "saved bitmap has the complete current pixel payload");
    check(BitmapArtifactPath(saved).ends_with("sharp-failure.bmp") &&
        BitmapArtifactPath(saved).find("sharp-sharp-") == std::string::npos,
        "artifact log path names the saved file without inventing another mode prefix");
    const auto blocked = root / L"blocked";
    { std::ofstream file(blocked); file << "sentinel"; }
    const auto directory_failure = SaveBitmapArtifact(pixels, 2, 2, blocked / L"image.bmp");
    check(!directory_failure.saved && !directory_failure.error.empty() && std::filesystem::file_size(blocked) == 8,
        "directory creation failure is explicit and preserves the blocking fixture file");
    const auto locked = root / L"locked.bmp";
    HANDLE lock = CreateFileW(locked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "fixture exclusively owns the output-open failure target");
    if (lock != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const char sentinel[] = "keep";
        const bool initialized = WriteFile(lock, sentinel, 4, &written, nullptr) && written == 4;
        const auto open_failure = SaveBitmapArtifact(pixels, 2, 2, locked);
        CloseHandle(lock);
        check(initialized && !open_failure.saved && !open_failure.error.empty() &&
            std::filesystem::file_size(locked) == 4, "output-open failure is explicit and does not claim an artifact");
    }
    const auto invalid = SaveBitmapArtifact({}, 2, 2, root / L"invalid.bmp");
    check(!invalid.saved && !invalid.error.empty() && !std::filesystem::exists(invalid.path),
        "invalid pixel input fails before creating a misleading output");
    image.close();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error && !std::filesystem::exists(root), "isolated artifact fixture is fully cleaned");
    if (!error) cleanup.path.clear();
    return failures ? 1 : 0;
}
