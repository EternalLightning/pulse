// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../preview_host/image_decoder.h"
#include "../preview_host/shell_thumbnail_bitmap.h"
#include <objbase.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <cwchar>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
int Colour(const std::vector<uint8_t>& pixels, UINT width, UINT x, UINT y) {
    const auto offset = (static_cast<size_t>(y) * width + x) * 4;
    const int b = pixels[offset], g = pixels[offset + 1], r = pixels[offset + 2];
    if (r > 170 && g < 80 && b < 80) return 0;
    if (g > 170 && r < 80 && b < 80) return 1;
    if (b > 170 && r < 80 && g < 80) return 2;
    if (r > 170 && g > 170 && b < 80) return 3;
    return -1;
}
void SavePpm(const std::filesystem::path& path, const std::vector<uint8_t>& pixels, UINT w, UINT h) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t at = 0; at < pixels.size(); at += 4) {
        const char rgb[] = {static_cast<char>(pixels[at + 2]), static_cast<char>(pixels[at + 1]),
            static_cast<char>(pixels[at])};
        file.write(rgb, 3);
    }
}
void BitmapRows() {
    for (const LONG signed_height : {-2L, 2L}) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 2;
        info.bmiHeader.biHeight = signed_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* memory = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
        Check(bitmap && memory, "#674 create real signed-height DIB");
        if (!bitmap || !memory) { if (bitmap) DeleteObject(bitmap); continue; }
        const uint8_t top[] = {16,16,240,255, 16,240,16,255};
        const uint8_t bottom[] = {240,16,16,255, 16,240,240,255};
        auto* bytes = static_cast<uint8_t*>(memory);
        std::memcpy(bytes, signed_height < 0 ? top : bottom, 8);
        std::memcpy(bytes + 8, signed_height < 0 ? bottom : top, 8);
        HBITMAP cached = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, 0));
        std::vector<uint8_t> pixels;
        UINT w = 0, h = 0, stride = 0;
        const bool cache_decoded = cached && pulse::preview::HbitmapToBgra(cached, true,
            pixels, w, h, stride, WTSAT_RGB);
        Check(cache_decoded && w == 2 && h == 2 && pixels.size() == 16 &&
            std::memcmp(pixels.data(), top, 8) == 0 && std::memcmp(pixels.data() + 8, bottom, 8) == 0,
            "#674 Shell cache CopyImage bitmap preserves row order");
        const bool decoded = pulse::preview::HbitmapToBgra(bitmap, true, pixels, w, h, stride, WTSAT_RGB);
        Check(decoded && w == 2 && h == 2 && stride == 8 && pixels.size() == 16 &&
            std::memcmp(pixels.data(), top, 8) == 0 && std::memcmp(pixels.data() + 8, bottom, 8) == 0,
            signed_height < 0 ? "#674 top-down Shell bitmap preserves row order" :
                "#674 bottom-up Shell bitmap preserves row order");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    BitmapRows();
    if (std::wcscmp(argv[1], L"--bitmap-only") == 0) {
        CoUninitialize();
        return failures ? 1 : 0;
    }
    const std::filesystem::path dir = argv[1];
    for (const auto* name : {L"large-no-exif.jpg", L"large-orientation-1.jpg"}) {
        std::vector<uint8_t> pixels;
        UINT w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        const bool decoded = pulse::preview::DecodeImage((dir / name).wstring(),
            FILE_ATTRIBUTE_NORMAL, 128, pixels, w, h, stride, sw, sh);
        Check(decoded && w == 85 && h == 128 && sw == 3911 && sh == 5866 &&
            Colour(pixels,w,w/4,h/4) == 0 && Colour(pixels,w,3*w/4,h/4) == 1 &&
            Colour(pixels,w,w/4,3*h/4) == 2 && Colour(pixels,w,3*w/4,3*h/4) == 3,
            "#674 large JPEG full-frame decoding keeps upright quadrant rows");
    }
    const int corners[8][4] = {{0,1,2,3}, {1,0,3,2}, {3,2,1,0}, {2,3,0,1},
        {0,2,1,3}, {2,0,3,1}, {3,1,2,0}, {1,3,0,2}};
    const auto base = dir / L"no-exif.jpg";
    std::ifstream jpeg(base, std::ios::binary);
    const std::vector<uint8_t> embedded((std::istreambuf_iterator<char>(jpeg)), {});
    for (uint32_t orientation = 1; orientation <= 8; ++orientation) {
        const auto path = dir / (L"orientation-" + std::to_wstring(orientation) + L".jpg");
        for (const UINT edge : {128u, 40u}) {
            std::vector<uint8_t> full, thumb;
            UINT w=0, h=0, stride=0, sw=0, sh=0, tw=0, th=0, ts=0, tsw=0, tsh=0;
            const bool decoded = pulse::preview::DecodeImage(path.wstring(), FILE_ATTRIBUTE_NORMAL, edge,
                full,w,h,stride,sw,sh);
            const bool explicit_orientation = pulse::preview::DecodeImage(base.wstring(), FILE_ATTRIBUTE_NORMAL, edge,
                thumb,tw,th,ts,tsw,tsh,&embedded,orientation);
            std::printf("[INFO] orientation=%u edge=%u decoded=%ux%u source=%ux%u\n", orientation,edge,w,h,sw,sh);
            const bool quarter = orientation >= 5;
            Check(decoded && explicit_orientation && full == thumb && w == tw && h == th &&
                sw == (quarter ? 48u : 80u) && sh == (quarter ? 80u : 48u),
                "full JPEG metadata orientation matches explicit embedded transform and source size");
            bool correct = decoded && !full.empty();
            if (correct) {
                const int actual[] = {Colour(full,w,w/4,h/4), Colour(full,w,3*w/4,h/4),
                    Colour(full,w,w/4,3*h/4), Colour(full,w,3*w/4,3*h/4)};
                for (int i=0;i<4;++i) correct &= actual[i] == corners[orientation-1][i];
                if (edge == 128) SavePpm(dir / (L"decoded-" + std::to_wstring(orientation) + L".ppm"),full,w,h);
            }
            Check(correct, "actual JPEG quadrant colours occupy the expected oriented corners");
        }
    }
    for (const auto* name : {L"no-exif.jpg", L"invalid-0.jpg", L"invalid-9.jpg", L"invalid-65535.jpg", L"fixture.png"}) {
        std::vector<uint8_t> pixels;
        UINT w=0,h=0,stride=0,sw=0,sh=0;
        Check(pulse::preview::DecodeImage((dir/name).wstring(),FILE_ATTRIBUTE_NORMAL,128,pixels,w,h,stride,sw,sh) &&
            w==80 && h==48 && sw==80 && sh==48 && Colour(pixels,w,w/4,h/4)==0,
            "missing/invalid orientation and ordinary PNG retain stored orientation");
    }
    CoUninitialize();
    std::printf("JPEG ORIENTATION failures=%d\n", failures);
    return failures ? 1 : 0;
}
