#pragma once
#include "../ui/ui_compositor.h"
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// Send PNGs to the fixture launcher when native UI processes cannot write files.
inline bool WriteAnimationSnapshot(pulse::ui::Compositor& compositor, const std::filesystem::path& path) {
    wchar_t transport[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", transport, ARRAYSIZE(transport)) != 1 || transport[0] != L'1')
        return compositor.SaveSnapshot(path.c_str());
    using Microsoft::WRL::ComPtr;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image;
    dc->GetTarget(&image);
    ComPtr<ID2D1Bitmap1> target, bitmap;
    if (!image || FAILED(image.As(&target))) return false;
    auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, properties, &bitmap)) ||
        FAILED(bitmap->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(bitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    const auto size = bitmap->GetPixelSize();
    std::vector<BYTE> pixels(static_cast<size_t>(mapped.pitch) * size.height);
    std::memcpy(pixels.data(), mapped.bits, pixels.size());
    const UINT pitch = mapped.pitch;
    bitmap->Unmap();
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size.width, size.height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(frame->WritePixels(size.height, pitch,
        static_cast<UINT>(pixels.size()), pixels.data())) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stat{};
    HGLOBAL memory = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return false;
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
    if (!bytes) return false;
    const size_t length = static_cast<size_t>(stat.cbSize.QuadPart);
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((length + 2) / 3 * 4);
    for (size_t i = 0; i < length; i += 3) {
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
            (i + 1 < length ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < length ? bytes[i + 2] : 0);
        encoded += alphabet[(value >> 18) & 63]; encoded += alphabet[(value >> 12) & 63];
        encoded += i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        encoded += i + 2 < length ? alphabet[value & 63] : '=';
    }
    GlobalUnlock(memory);
    std::printf("SNAPSHOT:%ls:%s\n", path.filename().c_str(), encoded.c_str());
    return !std::ferror(stdout);
}
