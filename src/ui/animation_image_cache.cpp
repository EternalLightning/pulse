// SPDX-License-Identifier: Apache-2.0
#include "animation_image_cache.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {

bool AnimationPixelCacheEnabled() {
    static const bool enabled = [] {
        wchar_t value[4]{};
        return GetEnvironmentVariableW(L"PULSE_ANIMATION_PIXEL_CACHE", value, ARRAYSIZE(value)) == 1 && value[0] == L'1';
    }();
    return enabled;
}

bool AnimationImageCache::Build(ID2D1DeviceContext* source, ID2D1CommandList* decoration,
    D2D1_RECT_F viewport, std::vector<AnimationImageSlice>& slices, ComPtr<ID2D1Bitmap1>& background,
    std::shared_ptr<ImageCacheCharge>& background_charge) {
    if (!AnimationPixelCacheEnabled()) return false;
    if (!context_.get()) {
        ComPtr<ID2D1Device> device;
        source->GetDevice(&device);
        if (!device.get() || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_))) return false;
    }
    auto* dc = context_.get();
    const UINT32 maximum = dc->GetMaximumBitmapSize();
    const UINT32 width = static_cast<UINT32>(std::max(1.0f, std::ceil(viewport.right - viewport.left)));
    const UINT32 height = static_cast<UINT32>(std::max(1.0f, std::ceil(viewport.bottom - viewport.top)));
    if (width > maximum || height > maximum || static_cast<uint64_t>(width) * height > 8u * 1024u * 1024u) return false;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    std::shared_ptr<ImageCacheCharge> incoming_charge;
    ComPtr<ID2D1Bitmap1> incoming;
    ComPtr<IDWriteRenderingParams> text_parameters;
    source->GetTextRenderingParams(&text_parameters);
    dc->SetTextRenderingParams(text_parameters.get());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (decoration) {
        incoming_charge = std::make_shared<ImageCacheCharge>(AnimationBitmapBudget());
        if (!incoming_charge->TrySetBytes(BitmapPayloadBytes(width, height))) return false;
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &incoming))) return false;
        dc->SetTarget(incoming.get());
        dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-viewport.left, -viewport.top));
        dc->DrawImage(decoration);
        const HRESULT background_result = dc->EndDraw();
        dc->SetTarget(nullptr);
        if (FAILED(background_result)) return false;
    }
    if (slices.empty()) {
        background = std::move(incoming);
        background_charge = std::move(incoming_charge);
        return true;
    }

    UINT32 total_width = 0, widest = 0;
    for (const auto& slice : slices) {
        const UINT32 w = static_cast<UINT32>(std::max(1.0f, std::ceil(slice.bounds.right - slice.bounds.left))) + 2;
        total_width += w; widest = std::max(widest, w);
    }
    const UINT32 atlas_width = std::max(widest, std::min(std::min(2048u, maximum), total_width));
    if (atlas_width > maximum) return false;
    UINT32 x = 0, y = 0, row_height = 0;
    for (auto& slice : slices) {
        const UINT32 w = static_cast<UINT32>(std::max(1.0f, std::ceil(slice.bounds.right - slice.bounds.left)));
        const UINT32 h = static_cast<UINT32>(std::max(1.0f, std::ceil(slice.bounds.bottom - slice.bounds.top)));
        if (x + w + 2 > atlas_width) { x = 0; y += row_height; row_height = 0; }
        slice.source = D2D1::RectF(static_cast<float>(x + 1), static_cast<float>(y + 1),
            static_cast<float>(x + 1) + slice.bounds.right - slice.bounds.left,
            static_cast<float>(y + 1) + slice.bounds.bottom - slice.bounds.top);
        x += w + 2; row_height = std::max(row_height, h + 2);
    }
    const UINT32 atlas_height = y + row_height;
    if (atlas_height > maximum || static_cast<uint64_t>(atlas_width) * atlas_height > 4u * 1024u * 1024u) return false;
    auto atlas_charge = std::make_shared<ImageCacheCharge>(AnimationBitmapBudget());
    if (!atlas_charge->TrySetBytes(BitmapPayloadBytes(atlas_width, atlas_height))) return false;
    ComPtr<ID2D1Bitmap1> atlas;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(atlas_width, atlas_height), nullptr, 0, properties, &atlas))) return false;
    dc->SetTarget(atlas.get());
    dc->BeginDraw(); dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    for (const auto& slice : slices) {
        dc->SetTransform(D2D1::Matrix3x2F::Translation(slice.source.left - slice.bounds.left,
            slice.source.top - slice.bounds.top));
        dc->PushAxisAlignedClip(slice.bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->DrawImage(slice.commands);
        dc->PopAxisAlignedClip();
    }
    const HRESULT atlas_result = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(atlas_result)) return false;
    background = std::move(incoming);
    background_charge = std::move(incoming_charge);
    for (auto& slice : slices) {
        atlas->AddRef(); slice.bitmap.p = atlas.get();
        slice.bitmap_charge = atlas_charge;
    }
    return true;
}

} // namespace pulse::ui
