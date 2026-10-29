#include "../preview_host/jpeg_embedded_preview.h"
#include "../preview_host/image_decoder.h"
#include "../fs/cloud_state.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace pulse::preview;
namespace {
int failures = 0;
int checks = 0;
void Check(bool value, const char* name) {
    ++checks;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
void Put16(std::vector<uint8_t>& out, size_t at, uint16_t value, bool little = true) {
    out[at + (little ? 0 : 1)] = static_cast<uint8_t>(value);
    out[at + (little ? 1 : 0)] = static_cast<uint8_t>(value >> 8);
}
void Put32(std::vector<uint8_t>& out, size_t at, uint32_t value, bool little = true) {
    for (size_t i = 0; i < 4; ++i) out[at + (little ? i : 3 - i)] = static_cast<uint8_t>(value >> (i * 8));
}
void Entry(std::vector<uint8_t>& out, size_t at, uint16_t tag, uint16_t type,
           uint32_t count, uint32_t value, bool little = true) {
    Put16(out, at, tag, little); Put16(out, at + 2, type, little);
    Put32(out, at + 4, count, little);
    if (type == 3 && count == 1) Put16(out, at + 8, static_cast<uint16_t>(value), little);
    else Put32(out, at + 8, value, little);
}
void Segment(std::vector<uint8_t>& jpeg, uint8_t marker, const std::vector<uint8_t>& body) {
    jpeg.push_back(0xFF); jpeg.push_back(marker);
    const uint16_t length = static_cast<uint16_t>(body.size() + 2);
    jpeg.push_back(static_cast<uint8_t>(length >> 8)); jpeg.push_back(static_cast<uint8_t>(length));
    jpeg.insert(jpeg.end(), body.begin(), body.end());
}
void Frame(std::vector<uint8_t>& jpeg, uint16_t w = 6000, uint16_t h = 4000) {
    Segment(jpeg, 0xC0, {8, static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
        static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w), 1, 1, 0x11, 0});
    Segment(jpeg, 0xDA, {1, 1, 0, 0, 63, 0});
}
std::vector<uint8_t> Embedded(const std::vector<uint8_t>& thumb, uint32_t orientation = 1,
                              bool little = true) {
    std::vector<uint8_t> tiff(56, 0);
    tiff[0] = tiff[1] = little ? 'I' : 'M';
    Put16(tiff, 2, 42, little); Put32(tiff, 4, 8, little);
    Put16(tiff, 8, 1, little); Entry(tiff, 10, 0x0112, 3, 1, orientation, little);
    Put32(tiff, 22, 26, little); Put16(tiff, 26, 2, little);
    Entry(tiff, 28, 0x0201, 4, 1, 56, little);
    Entry(tiff, 40, 0x0202, 4, 1, static_cast<uint32_t>(thumb.size()), little);
    tiff.insert(tiff.end(), thumb.begin(), thumb.end());
    std::vector<uint8_t> body{'E', 'x', 'i', 'f', 0, 0};
    body.insert(body.end(), tiff.begin(), tiff.end());
    std::vector<uint8_t> jpeg{0xFF, 0xD8}; Segment(jpeg, 0xE1, body); Frame(jpeg);
    return jpeg;
}
std::vector<uint8_t> Mpf(uint32_t offset, uint32_t bytes, uint32_t type = 0x010001) {
    std::vector<uint8_t> tiff(42, 0);
    tiff[0] = tiff[1] = 'I'; Put16(tiff, 2, 42); Put32(tiff, 4, 8);
    Put16(tiff, 8, 1); Entry(tiff, 10, 0xB002, 7, 16, 26);
    Put32(tiff, 26, type); Put32(tiff, 30, bytes); Put32(tiff, 34, offset);
    std::vector<uint8_t> body{'M', 'P', 'F', 0}; body.insert(body.end(), tiff.begin(), tiff.end());
    std::vector<uint8_t> jpeg{0xFF, 0xD8}; Segment(jpeg, 0xE2, body); Frame(jpeg);
    return jpeg;
}
bool Save(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size();
    CloseHandle(file); return ok;
}
std::vector<uint8_t> EncodeJpeg() {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(300, 200))) return {};
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) return {};
    std::vector<uint8_t> pixels(300 * 200 * 3, 0);
    for (size_t i = 0; i < pixels.size(); i += 3) { pixels[i] = 24; pixels[i + 1] = 96; pixels[i + 2] = 210; }
    if (FAILED(frame->WritePixels(200, 900, static_cast<UINT>(pixels.size()), pixels.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return {};
    STATSTG stat{};
    LARGE_INTEGER start{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.HighPart ||
        FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return {};
    std::vector<uint8_t> bytes(stat.cbSize.LowPart);
    ULONG got = 0;
    if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &got)) || got != bytes.size()) return {};
    return bytes;
}
std::vector<uint8_t> OrientationBitmap() {
    std::vector<uint8_t> bmp(54 + 24, 0);
    bmp[0] = 'B'; bmp[1] = 'M'; Put32(bmp, 2, static_cast<uint32_t>(bmp.size()));
    Put32(bmp, 10, 54); Put32(bmp, 14, 40); Put32(bmp, 18, 3); Put32(bmp, 22, 2);
    Put16(bmp, 26, 1); Put16(bmp, 28, 24); Put32(bmp, 34, 24);
    // Bottom-up BMP. Each pixel's red channel is its logical row-major ID.
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 3; ++x)
            bmp[54 + (1 - y) * 12 + x * 3 + 2] = static_cast<uint8_t>(1 + y * 3 + x);
    return bmp;
}
}

int wmain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 2;
    const auto dir = std::filesystem::path(L"bench_data") /
        (L"jpeg_embedded_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    const auto path = (dir / L"fixture.jpg").wstring();
    const auto thumb = EncodeJpeg();
    Check(!thumb.empty(), "fixture: real WIC JPEG encoded");
    if (thumb.empty()) { CoUninitialize(); return 2; }
    const auto jpeg = Embedded(thumb, 6);
    JpegHeaderInfo info;
    Check(ParseJpegHeader(jpeg.data(), jpeg.size(), info) && info.width == 6000 && info.height == 4000 &&
        info.orientation == 6 && info.exif_thumbnail_size == thumb.size(), "Exif offsets, source size and orientation");
    bool prefixes_safe = true;
    for (size_t length = 0; length < jpeg.size(); ++length)
        if (ParseJpegHeader(jpeg.data(), length, info)) prefixes_safe = false;
    Check(prefixes_safe, "every truncated header rejected");
    Check(!ParseJpegHeader(nullptr, 128, info), "null header rejected");
    UINT w = 1, h = 1, stride = 1, sw = 1, sh = 1;
    Check(!JpegFrameSize(nullptr, 100, w, h) && w == 0 && h == 0, "null frame rejected and dimensions cleared");
    auto malformed = jpeg;
    Put32(malformed, 16, 0xFFFFFFFE);
    Check(ParseJpegHeader(malformed.data(), malformed.size(), info) && info.exif_thumbnail_size == 0,
          "wrapped TIFF IFD offset cannot expose embedded bytes");
    malformed = jpeg; Put32(malformed, 48, 0xFFFFFFF0);
    Check(ParseJpegHeader(malformed.data(), malformed.size(), info) && info.exif_thumbnail_size == 0,
          "out of range Exif thumbnail offset ignored");
    malformed = jpeg; Put16(malformed, 24, 4);
    Check(ParseJpegHeader(malformed.data(), malformed.size(), info) && info.orientation == 1,
          "wrong TIFF orientation field type ignored");
    const auto big = Embedded(thumb, 8, false);
    Check(ParseJpegHeader(big.data(), big.size(), info) && info.orientation == 8 && info.exif_thumbnail_size == thumb.size(),
          "big endian Exif fields");
    JpegEmbeddedPreview result;
    Check(Save(path, jpeg) && ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result) &&
        result.jpeg == thumb && result.source_width == 6000 && result.orientation == 6,
        "camera Exif preview selected without decoding main image");
    std::vector<uint8_t> output;
    Check(DecodeImage(path, FILE_ATTRIBUTE_NORMAL, 256, output, w, h, stride, sw, sh, &result.jpeg, result.orientation) &&
        w == 171 && h == 256 && stride == 684 && output.size() == size_t(stride) * h && sw == 200 && sh == 300,
        "real embedded JPEG decoded, scaled and rotated");
    Check(!ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 512, result) && result.jpeg.empty(),
          "small embedded preview rejected with clean output");
    Check(!ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 0, result) &&
        !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, UINT_MAX, result), "requested edge bounded");
    std::vector<uint8_t> square{0xFF, 0xD8}; Frame(square, 300, 300);
    Check(Save(path, Embedded(square)) && !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result),
          "letterboxed or different aspect preview rejected");
    auto icc = jpeg;
    std::vector<uint8_t> profile{'I','C','C','_','P','R','O','F','I','L','E',0,1,1};
    std::vector<uint8_t> segment; Segment(segment, 0xE2, profile);
    icc.insert(icc.begin() + 2, segment.begin(), segment.end());
    Check(Save(path, icc) && !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result),
          "profiled JPEG falls back to original decoder");
    auto mpf = Mpf(0, static_cast<uint32_t>(thumb.size()));
    const uint32_t thumb_offset = static_cast<uint32_t>(mpf.size());
    mpf = Mpf(thumb_offset - 10, static_cast<uint32_t>(thumb.size()));
    mpf.insert(mpf.end(), thumb.begin(), thumb.end());
    Check(Save(path, mpf) && ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result) && result.jpeg == thumb,
          "MPF camera large preview reads absolute TIFF-relative offset");
    Check(Save(path, Mpf(UINT_MAX, static_cast<uint32_t>(thumb.size()))) &&
        !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result), "MPF beyond EOF rejected");
    Check(Save(path, Mpf(80, 9u * 1024 * 1024)) &&
        !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result), "oversized MPF preview rejected");
    Check(Save(path, Mpf(80, 200, 0x030000)) &&
        !ReadJpegEmbeddedPreview(path, FILE_ATTRIBUTE_NORMAL, 256, result), "MPF depth or alternate full image ignored");
    for (const DWORD attr : std::array<DWORD, 3>{FILE_ATTRIBUTE_OFFLINE, pulse::fs::kAttrRecallOnOpen,
                                               pulse::fs::kAttrRecallOnDataAccess}) {
        SetLastError(0x12345678);
        const bool read = ReadJpegEmbeddedPreview(path, attr, 256, result);
        Check(!read && result.jpeg.empty() && GetLastError() == 0x12345678,
              "cloud placeholder attributes reject before any file API");
    }
    output = {1,2,3};
    Check(!DecodeImage(path, FILE_ATTRIBUTE_NORMAL, UINT_MAX, output, w, h, stride, sw, sh, &thumb) &&
        output.empty() && w == 0 && h == 0, "decoder rejects oversized pixels and clears output");
    const std::vector<uint8_t> invalid{0xFF, 0xD8, 0};
    Check(!DecodeImage(path, FILE_ATTRIBUTE_NORMAL, 256, output, w, h, stride, sw, sh, &invalid),
          "malformed embedded JPEG rejected so full decode can continue");
    Check(!DecodeImage(path, FILE_ATTRIBUTE_OFFLINE, 256, output, w, h, stride, sw, sh, &thumb),
          "decoder independently rejects cloud placeholders");
    const auto bitmap = OrientationBitmap();
    constexpr std::array<std::array<uint8_t, 6>, 8> expected{{
        {{1,2,3,4,5,6}}, {{3,2,1,6,5,4}}, {{6,5,4,3,2,1}}, {{4,5,6,1,2,3}},
        {{1,4,2,5,3,6}}, {{4,1,5,2,6,3}}, {{6,3,5,2,4,1}}, {{3,6,2,5,1,4}}
    }};
    for (uint32_t orientation = 1; orientation <= 8; ++orientation) {
        const bool quarter = orientation >= 5;
        Check(DecodeImage(path, FILE_ATTRIBUTE_NORMAL, 300, output, w, h, stride, sw, sh, &thumb, orientation) &&
            w == (quarter ? 200u : 300u) && h == (quarter ? 300u : 200u), "all eight Exif orientation sizes");
        bool correct = DecodeImage(path, FILE_ATTRIBUTE_NORMAL, 32, output, w, h, stride, sw, sh, &bitmap, orientation);
        correct = correct && output.size() == 24 && w == (quarter ? 2u : 3u) && h == (quarter ? 3u : 2u);
        for (size_t i = 0; correct && i < 6; ++i) correct = output[i * 4 + 2] == expected[orientation - 1][i];
        Check(correct, "all eight Exif orientations preserve exact pixel positions");
    }
    Check(Save(path, thumb) && DecodeImage(path, FILE_ATTRIBUTE_NORMAL, 256, output, w, h, stride, sw, sh) &&
        w == 256 && h == 171 && sw == 300 && sh == 200, "ordinary file decoding remains available after embedded miss");
    std::filesystem::remove(path);
    std::filesystem::remove(dir);
    CoUninitialize();
    std::printf("JPEG EMBEDDED checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
