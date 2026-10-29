#include "image_decoder.h"
#include "../fs/cloud_state.h"
#include "../ipc/preview_protocol.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>

namespace pulse::preview {
using Microsoft::WRL::ComPtr;

bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                 std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
                 UINT& source_width, UINT& source_height,
                 const std::vector<uint8_t>* embedded, uint32_t orientation) {
    out.clear();
    width = height = stride = source_width = source_height = 0;
    if (fs::IsCloudPlaceholder(attrs) || !pixels || pixels > ipc::kPreviewMaxPixelSize ||
        orientation < 1 || orientation > 8) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmap> held;
    ComPtr<IWICBitmapFlipRotator> rotation, reflection;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    if (embedded) {
        if (embedded->empty() || embedded->size() > 8u * 1024 * 1024 ||
            FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(embedded->data()),
                                                static_cast<DWORD>(embedded->size()))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    } else if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    UINT source_w = 0, source_h = 0;
    if (FAILED(frame->GetSize(&source_w, &source_h)) || !source_w || !source_h) return false;
    IWICBitmapSource* source = frame.Get();
    const UINT longest = (std::max)(source_w, source_h);
    if (longest > pixels) {
        const double ratio = static_cast<double>(pixels) / longest;
        const UINT scaled_w = (std::max)(1u, static_cast<UINT>(source_w * ratio + 0.5));
        const UINT scaled_h = (std::max)(1u, static_cast<UINT>(source_h * ratio + 0.5));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), scaled_w, scaled_h,
                                      WICBitmapInterpolationModeFant))) return false;
        source = scaler.Get();
    }
    // Materialize the scaled bitmap before rotating, so the decoder never
    // has to decode the full camera image again for each rotated scanline.
    if (orientation != 1) {
        WICBitmapTransformOptions transform = WICBitmapTransformRotate0;
        switch (orientation) {
        case 2: transform = WICBitmapTransformFlipHorizontal; break;
        case 3: transform = WICBitmapTransformRotate180; break;
        case 4: transform = WICBitmapTransformFlipVertical; break;
        case 5: case 6: case 7: transform = WICBitmapTransformRotate90; break;
        case 8: transform = WICBitmapTransformRotate270; break;
        }
        if (FAILED(factory->CreateBitmapFromSource(source, WICBitmapCacheOnLoad, &held)) ||
            FAILED(factory->CreateBitmapFlipRotator(&rotation)) ||
            FAILED(rotation->Initialize(held.Get(), transform))) return false;
        source = rotation.Get();
        if (orientation == 5 || orientation == 7) {
            if (FAILED(factory->CreateBitmapFlipRotator(&reflection)) ||
                FAILED(reflection->Initialize(source, orientation == 5
                    ? WICBitmapTransformFlipHorizontal : WICBitmapTransformFlipVertical))) return false;
            source = reflection.Get();
        }
    }
    UINT result_w = 0, result_h = 0;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&result_w, &result_h)) || !result_w || !result_h ||
        result_w > pixels || result_h > pixels) return false;
    const UINT result_stride = result_w * 4;
    out.resize(static_cast<size_t>(result_stride) * result_h);
    if (FAILED(converter->CopyPixels(nullptr, result_stride, static_cast<UINT>(out.size()), out.data()))) {
        out.clear();
        return false;
    }
    width = result_w; height = result_h; stride = result_stride;
    source_width = orientation >= 5 ? source_h : source_w;
    source_height = orientation >= 5 ? source_w : source_h;
    return true;
}
}
