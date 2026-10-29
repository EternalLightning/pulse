#pragma once

#include "../ui/ui_compositor.h"
#include <objidl.h>
#include <wincodec.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace pulse::bench {

// Native fixtures can stream their PNGs to the invoking harness when the
// execution environment permits the harness, but not its children, to save files.
inline bool StreamSnapshots() {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"PULSE_TEST_STREAM_SNAPSHOTS", value, 2) == 1 && value[0] == L'1';
}

inline bool EmitSnapshot(const std::filesystem::path& path, UINT width, UINT height,
                         UINT stride, BYTE* pixels) {
    using namespace ui;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA,
            stride, stride * height, pixels, &bitmap)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WriteSource(bitmap.get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stat{};
    HGLOBAL global = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.HighPart ||
        FAILED(GetHGlobalFromStream(stream.get(), &global))) return false;
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(global));
    if (!bytes) return false;
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    const size_t count = stat.cbSize.LowPart;
    encoded.reserve((count + 2) / 3 * 4);
    for (size_t i = 0; i < count; i += 3) {
        const auto value = (static_cast<unsigned>(bytes[i]) << 16) |
            (i + 1 < count ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < count ? bytes[i + 2] : 0);
        encoded += alphabet[value >> 18];
        encoded += alphabet[(value >> 12) & 63];
        encoded += i + 1 < count ? alphabet[(value >> 6) & 63] : '=';
        encoded += i + 2 < count ? alphabet[value & 63] : '=';
    }
    GlobalUnlock(global);
    const auto wide_path = path.wstring();
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return false;
    std::string utf8_path(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide_path.c_str(), -1, utf8_path.data(), length, nullptr, nullptr);
    std::printf("PULSE_PNG|%s|%s\n", utf8_path.c_str(), encoded.c_str());
    return true;
}

inline bool SaveSnapshot(ui::Compositor& compositor, const std::filesystem::path& path) {
    if (!StreamSnapshots()) return compositor.SaveSnapshot(path.c_str());
    auto* dc = compositor.Dc();
    if (!dc) return false;
    ui::ComPtr<ID2D1Image> target;
    ui::ComPtr<ID2D1Bitmap1> source, cpu;
    dc->GetTarget(&target);
    if (!target.get() || FAILED(target->QueryInterface(&source))) return false;
    const auto size = source->GetPixelSize();
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, source->GetPixelFormat(), 96, 96);
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, properties, &cpu)) ||
        FAILED(cpu->CopyFromBitmap(nullptr, source.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    const bool saved = EmitSnapshot(path, size.width, size.height, mapped.pitch, mapped.bits);
    cpu->Unmap();
    return saved;
}

} // namespace pulse::bench
