// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "svg_bitmap.h"
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <shlwapi.h>
#include <wrl/client.h>

namespace pulse::ui {
HRESULT CreateSvgResourceBitmap(ID2D1RenderTarget* target, int resource_id,
                                D2D1_SIZE_U size, D2D1_SIZE_F viewport, ID2D1Bitmap** bitmap) {
    using Microsoft::WRL::ComPtr;
    if (!target || !bitmap || viewport.width <= 0 || viewport.height <= 0) return E_INVALIDARG;
    *bitmap = nullptr;
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto bytes = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    if (!bytes) return E_FAIL;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes, SizeofResource(module, resource)));
    if (!stream) return E_OUTOFMEMORY;
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(hr)) hr = d3d.As(&dxgi);
    ComPtr<ID2D1Device> device;
    if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.Get(), nullptr, &device);
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    ComPtr<ID2D1DeviceContext5> svg_context;
    if (SUCCEEDED(hr)) hr = context.As(&svg_context);
    ComPtr<ID2D1SvgDocument> svg;
    if (SUCCEEDED(hr)) hr = svg_context->CreateSvgDocument(stream.Get(), viewport, &svg);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1SvgElement> background;
    if (SUCCEEDED(svg->FindElementById(L"background", &background)) && background)
        background->SetAttributeValue(L"display", D2D1_SVG_DISPLAY_NONE);
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> rendered, readable;
    hr = context->CreateBitmap(size, nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format), &rendered);
    if (FAILED(hr)) return hr;
    context->SetTarget(rendered.Get());
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0.0f));
    context->SetTransform(D2D1::Matrix3x2F::Scale(size.width / viewport.width, size.height / viewport.height));
    svg_context->DrawSvgDocument(svg.Get());
    hr = context->EndDraw();
    context->SetTarget(nullptr);
    if (SUCCEEDED(hr)) hr = context->CreateBitmap(size, nullptr, 0,
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format), &readable);
    if (SUCCEEDED(hr)) hr = readable->CopyFromBitmap(nullptr, rendered.Get(), nullptr);
    D2D1_MAPPED_RECT mapped{};
    if (SUCCEEDED(hr)) hr = readable->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) return hr;
    hr = target->CreateBitmap(size, mapped.bits, mapped.pitch, D2D1::BitmapProperties(format), bitmap);
    readable->Unmap();
    return hr;
}
}
