// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "fluent_components.h"
#include "typography.h"
#include "../common/path_utils.h"

namespace pulse::ui {

inline void DrawConflictText(Compositor& compositor, std::wstring_view text,
    const D2D1_RECT_F& bounds, D2D1_COLOR_F color, bool heading = false, bool wrap = false) {
    if (text.empty() || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    std::wstring value(text);
    for (auto& ch : value) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    auto* format = heading ? compositor.TextFormat() : compositor.SmallFormat();
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(typography::CreateTextLayout(compositor.DwriteFactory(), value.c_str(), static_cast<UINT32>(value.size()),
        format, bounds.right - bounds.left, bounds.bottom - bounds.top, &layout))) return;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<IDWriteInlineObject> ellipsis;
    compositor.DwriteFactory()->CreateEllipsisTrimmingSign(format, &ellipsis);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, ellipsis.get());
    if (!wrap && !IsHighContrast() && compositor.DrawLumaText(value, layout.get(), bounds,
        color, HexColor(0x000000, 0), DWRITE_TEXT_ALIGNMENT_LEADING)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(compositor.Dc()->CreateSolidColorBrush(color, &brush)))
        compositor.Dc()->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

struct ConflictDetailsColumn {
    std::wstring_view label, metadata, path;
};

inline void DrawConflictDetails(fluent::Painter& painter, Compositor& compositor,
    const D2D1_RECT_F& bounds, float scale, const Theme& theme, bool dark, bool columns,
    const ConflictDetailsColumn& source, const ConflictDetailsColumn& existing) {
    const bool high_contrast = IsHighContrast();
    painter.FillRoundedRect(bounds, 7 * scale, high_contrast ? theme.fill_input :
        dark ? HexColor(0x161616, 0.62f) : HexColor(0xFFFFFF, 0.62f));
    painter.StrokeRoundedRect(bounds, 7 * scale, theme.stroke_card);
    const float middle = (bounds.left + bounds.right) * 0.5f;
    for (const bool is_source : {true, false}) {
        const auto& column = is_source ? source : existing;
        const float left = !is_source && columns ? middle + 10 * scale : bounds.left + 12 * scale;
        const float right = is_source && columns ? middle - 10 * scale : bounds.right - 12 * scale;
        const float top = bounds.top + (!is_source && !columns ? 104 * scale : 0);
        DrawConflictText(compositor, column.label, D2D1::RectF(left, top + 8 * scale, right, top + 28 * scale),
            is_source ? theme.accent : theme.text_secondary);
        DrawConflictText(compositor, column.metadata, D2D1::RectF(left, top + 30 * scale, right, top + 50 * scale), theme.text);
        const auto display_path = path::StripExtendedPathPrefix(column.path);
        DrawConflictText(compositor, display_path, D2D1::RectF(left, top + 55 * scale, right, top + 93 * scale),
            theme.text_secondary, false, true);
    }
}

} // namespace pulse::ui
