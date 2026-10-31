// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../preview_host/image_decoder.h"
#include <objbase.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

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
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const std::filesystem::path dir = argv[1];
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
