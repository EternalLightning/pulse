#pragma once
#include <windows.h>
#include <thumbcache.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

namespace pulse::preview {
using Microsoft::WRL::ComPtr;
inline bool HbitmapToBgra(HBITMAP bitmap, bool own, std::vector<uint8_t>& out,
                          UINT& width, UINT& height, UINT& stride, WTS_ALPHATYPE alpha = WTSAT_UNKNOWN) {
    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory)))) {
        if (own) DeleteObject(bitmap);
        return false;
    }
    const WICBitmapAlphaChannelOption options[] = {
        alpha == WTSAT_RGB ? WICBitmapIgnoreAlpha : WICBitmapUsePremultipliedAlpha,
        WICBitmapIgnoreAlpha
    };
    bool ok = false;
    for (const auto option : options) {
        ComPtr<IWICBitmap> source;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateBitmapFromHBITMAP(bitmap, nullptr, option, &source)))
            continue;
        if (FAILED(wicFactory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
            continue;
        stride = width * 4;
        out.resize(static_cast<size_t>(stride) * height);
        if (SUCCEEDED(converter->CopyPixels(nullptr, stride,
                static_cast<UINT>(out.size()), out.data()))) {
            ok = true;
            break;
        }
        out.clear();
    }
    if (own) DeleteObject(bitmap);
    if (!ok) out.clear();
    return ok;
}

}
