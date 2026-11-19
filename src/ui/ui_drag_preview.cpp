// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "command_icons.h"
#include "ui_renderer_internal.h"
#include <cstring>

namespace pulse::ui {

HBITMAP MainRenderer::CreateDragPreview(const PaneViewModel& pane, int index, const Theme& theme) {
    const auto source_count = pane.content_results ? pane.content_results->Count() :
        pane.snapshot ? pane.snapshot->size() : pane.entries.size();
    if (!compositor_ || !compositor_->Dc() || index < 0 ||
        static_cast<size_t>(index) >= source_count) return nullptr;
    const auto& entry = MakeVisibleEntry(pane, static_cast<size_t>(index));
    if (entry.name.empty()) return nullptr;

    const auto mode = pane.view_mode;
    const bool icon_only = mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons ||
        mode == ViewMode::MediumIcons || mode == ViewMode::SmallIcons;
    const auto layout = ViewLayout(mode, {}, 1, 0, 0, scale_);
    const float icon_size = std::round(layout.Metrics().icon_size);
    const float padding = std::round(6 * scale_);
    const float text_left = icon_size + 2 * padding;
    auto* format = compositor_->FileNameFormat();
    auto* factory = compositor_->DwriteFactory();
    if (!icon_only && (!format || !factory)) return nullptr;
    const auto name = icon_only ? std::wstring{} :
        FitFileName(compositor_, factory, format, entry.name, 240 * scale_);
    const float text_width = icon_only ? 0 : MeasureLayoutText(compositor_, factory, format, name);
    const auto size = D2D1::SizeU(
        static_cast<UINT32>(std::ceil(icon_only ? icon_size + 2 * padding : text_left + text_width + padding)),
        static_cast<UINT32>(std::ceil(icon_size + 2 * padding)));

    auto* dc = compositor_->Dc();
    const auto pixel_format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> image, readable;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixel_format, 96, 96), &image)) ||
        FAILED(dc->CreateBitmap(size, nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                pixel_format, 96, 96), &readable))) return nullptr;

    ComPtr<ID2D1Image> original_target;
    D2D1_MATRIX_3X2_F original_transform{};
    dc->GetTarget(&original_target);
    dc->GetTransform(&original_transform);
    dc->SetTarget(image.get());
    dc->BeginDraw();
    dc->SetTransform(D2D1::IdentityMatrix());
    dc->Clear(D2D1::ColorF(0, 0.0f));
    ComPtr<ID2D1SolidColorBrush> brush;
    MakeBrush(dc, theme.surface_flyout, brush);
    FillRoundedRect(dc, brush.get(), 0, 0, static_cast<float>(size.width),
        static_cast<float>(size.height), theme.radius_control * scale_);
    const auto icon = D2D1::RectF(padding, padding, padding + icon_size, padding + icon_size);
    const auto requested = mode == ViewMode::LargeIcons || mode == ViewMode::ExtraLargeIcons ? 256u :
        mode == ViewMode::MediumIcons ? 128u : static_cast<UINT32>(std::max(32.0f, icon_size));
    const bool thumbnail_drawn = UsesThumbnails(mode) && thumbnail_cache_.Draw(dc, icon, entry.path, entry.attrs,
        requested, pane.view_generation, entry.modified_value, entry.size_value) == PreviewDrawResult::Bitmap;
    if (!thumbnail_drawn && !icon_cache_.Draw(dc, icon, entry.path, entry.name, entry.is_dir, entry.attrs)) {
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, entry.is_dir ? theme.icon_folder : theme.icon_file, brush);
        command_icons::Draw(dc, brush.get(), paneHeaderStroke_.get(),
            entry.is_dir ? command_icons::Icon::Folder : command_icons::Icon::File, icon);
    }
    if (!name.empty()) {
        MakeBrush(dc, theme.text, brush);
        DrawTextRect(dc, format, brush.get(), name, text_left, 0,
            static_cast<float>(size.width) - text_left, static_cast<float>(size.height),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, true);
    }
    const HRESULT drawn = dc->EndDraw();
    dc->SetTransform(original_transform);
    dc->SetTarget(original_target.get());
    if (FAILED(drawn) || FAILED(readable->CopyFromBitmap(nullptr, image.get(), nullptr))) return nullptr;

    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(size.width),
        -static_cast<LONG>(size.height), 1, 32, BI_RGB};
    void* pixels = nullptr;
    const HBITMAP result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!result) return nullptr;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
        DeleteObject(result);
        return nullptr;
    }
    const size_t stride = size.width * 4;
    for (UINT32 row = 0; row < size.height; ++row)
        std::memcpy(static_cast<BYTE*>(pixels) + row * stride, mapped.bits + row * mapped.pitch, stride);
    readable->Unmap();
    return result;
}

} // namespace pulse::ui
