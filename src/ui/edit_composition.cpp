// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "edit_composition.h"
#include <dcomp.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <unordered_map>

namespace pulse::ui {
using Microsoft::WRL::ComPtr;

struct EditComposition::Impl {
    struct Entry {
        ComPtr<IDCompositionVisual> visual;
        ComPtr<IDCompositionSurface> surface;
        ComPtr<IDCompositionEffectGroup> opacity;
        UINT width = 0, height = 0;
    };
    HWND host;
    ComPtr<IDCompositionDevice> device;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;
    ComPtr<ID2D1DeviceContext> dc;
    std::unordered_map<HWND, Entry> entries;

    Impl(HWND window, IDCompositionDevice* composition, ID2D1Device* drawing,
         IDCompositionEffectGroup* effect) : host(window), device(composition) {
        if (!composition || !drawing) return;
        HRESULT hr = drawing->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);
        // The main target is below child HWNDs. This second target owns only
        // editor rectangles above them, so EDIT's immediate GDI paints stay covered.
        if (SUCCEEDED(hr)) hr = device->CreateTargetForHwnd(host, TRUE, &target);
        if (SUCCEEDED(hr)) hr = device->CreateVisual(&root);
        if (SUCCEEDED(hr) && effect) hr = root->SetEffect(effect);
        if (SUCCEEDED(hr)) hr = target->SetRoot(root.Get());
        if (FAILED(hr)) {
            target.Reset();
            root.Reset();
            dc.Reset();
        } else {
            dc->SetDpi(96.0f, 96.0f);
        }
    }
    ~Impl() {
        for (const auto& [edit, entry] : entries) {
            (void)entry;
            RemovePropW(edit, L"Pulse.EditComposition");
        }
        if (target) target->SetRoot(nullptr);
        if (device) device->Commit();
    }
    bool Place(HWND edit, Entry& entry, UINT width, UINT height) {
        auto* visual = entry.visual.Get();
        const BYTE alpha = EditSurfaceOpacity(edit);
        if (alpha < 255) {
            if ((!entry.opacity && FAILED(device->CreateEffectGroup(&entry.opacity))) ||
                FAILED(entry.opacity->SetOpacity(alpha / 255.0f)) ||
                FAILED(visual->SetEffect(entry.opacity.Get()))) return false;
        } else if (entry.opacity) {
            if (FAILED(visual->SetEffect(nullptr))) return false;
            entry.opacity.Reset();
        }
        POINT origin{};
        if (!ClientToScreen(edit, &origin) || !ScreenToClient(host, &origin)) return false;
        const D2D_RECT_F clip{0, 0, static_cast<float>(width), static_cast<float>(height)};
        return SUCCEEDED(visual->SetOffsetX(static_cast<float>(origin.x))) &&
            SUCCEEDED(visual->SetOffsetY(static_cast<float>(origin.y))) &&
            SUCCEEDED(visual->SetClip(clip));
    }
};

EditComposition::EditComposition(HWND host, IDCompositionDevice* device, ID2D1Device* drawing,
                                 IDCompositionEffectGroup* effect)
    : impl_(std::make_unique<Impl>(host, device, drawing, effect)) {}
EditComposition::~EditComposition() = default;
bool EditComposition::Available() const { return impl_->root && impl_->dc; }

bool EditComposition::Present(HWND edit, ID2D1Bitmap1* bitmap) {
    if (!Available() || !bitmap || !IsWindowVisible(edit)) return false;
    auto& state = *impl_;
    const auto size = bitmap->GetPixelSize();
    if (!size.width || !size.height) return false;
    auto found = state.entries.find(edit);
    const bool replace = found == state.entries.end() || found->second.width != size.width ||
        found->second.height != size.height;
    Impl::Entry next;
    Impl::Entry* entry = replace ? &next : &found->second;
    HRESULT hr = S_OK;
    if (replace) {
        next.width = size.width; next.height = size.height;
        hr = state.device->CreateSurface(size.width, size.height, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &next.surface);
        if (SUCCEEDED(hr)) hr = state.device->CreateVisual(&next.visual);
        if (SUCCEEDED(hr)) hr = next.visual->SetContent(next.surface.Get());
    }
    if (FAILED(hr)) return false;
    ComPtr<IDXGISurface> texture;
    POINT offset{};
    hr = entry->surface->BeginDraw(nullptr, IID_PPV_ARGS(&texture), &offset);
    if (FAILED(hr)) return false;
    ComPtr<ID2D1Bitmap1> target;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    hr = state.dc->CreateBitmapFromDxgiSurface(texture.Get(), &properties, &target);
    if (SUCCEEDED(hr)) {
        state.dc->SetTarget(target.Get());
        state.dc->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x), static_cast<float>(offset.y)));
        state.dc->BeginDraw();
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(size.width), static_cast<float>(size.height));
        state.dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
        state.dc->Clear(D2D1::ColorF(0, 0.0f));
        state.dc->DrawBitmap(bitmap, bounds, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        state.dc->PopAxisAlignedClip();
        hr = state.dc->EndDraw();
        state.dc->SetTarget(nullptr);
    }
    target.Reset(); texture.Reset();
    const HRESULT ended = entry->surface->EndDraw();
    if (FAILED(hr) || FAILED(ended) || !state.Place(edit, *entry, size.width, size.height)) return false;
    if (replace) {
        if (FAILED(state.root->AddVisual(next.visual.Get(), TRUE, nullptr))) return false;
        if (found != state.entries.end()) state.root->RemoveVisual(found->second.visual.Get());
        state.entries.insert_or_assign(edit, std::move(next));
    }
    if (FAILED(state.device->Commit())) return false;
    SetPropW(edit, L"Pulse.EditComposition", reinterpret_cast<HANDLE>(1));
    return true;
}

void EditComposition::Synchronize(HWND edit) {
    auto& state = *impl_;
    auto found = state.entries.find(edit);
    if (found == state.entries.end()) return;
    if (!IsWindowVisible(edit)) { Remove(edit); return; }
    if (state.Place(edit, found->second, found->second.width, found->second.height))
        state.device->Commit();
}
void EditComposition::Remove(HWND edit) {
    RemovePropW(edit, L"Pulse.EditComposition");
    auto& state = *impl_;
    const auto found = state.entries.find(edit);
    if (found == state.entries.end()) return;
    state.root->RemoveVisual(found->second.visual.Get());
    state.entries.erase(found);
    state.device->Commit();
}
}
