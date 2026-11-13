// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once
#include "typography.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {

inline D2D1_RECT_F MakeTooltipLayout(IDWriteFactory2* factory, IDWriteTextFormat* format,
    std::wstring_view text, float scale, const D2D1_RECT_F& window, float x, float y,
    float status_height, bool above_status, IDWriteTextLayout** result) {
    *result = nullptr;
    const float margin = 8.0f * scale, pad_x = 10.0f * scale, pad_y = 6.0f * scale;
    const float width = std::min(520.0f * scale, window.right - window.left - 2 * margin);
    const float height = window.bottom - window.top - 2 * margin;
    if (!factory || !format || width <= 2 * pad_x || height <= 2 * pad_y) return {};
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(typography::CreateTextLayout(factory, text.data(), static_cast<UINT32>(text.size()),
        format, width - 2 * pad_x, height - 2 * pad_y, &layout))) return {};
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    IDWriteInlineObject* ellipsis = nullptr;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis);
        ellipsis->Release();
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) { layout->Release(); return {}; }
    const float bw = std::min(width, std::ceil(metrics.widthIncludingTrailingWhitespace) + 2 * pad_x);
    const float bh = std::min(height, std::max(28.0f * scale, std::ceil(metrics.height) + 2 * pad_y));
    const float bx = std::clamp(x + 12.0f * scale, window.left + margin, window.right - margin - bw);
    const float tip_y = above_status ? window.bottom - status_height - bh - margin : y + 18.0f * scale;
    const float by = std::clamp(tip_y, window.top + margin, window.bottom - margin - bh);
    layout->SetMaxWidth(std::max(1.0f, bw - 2 * pad_x));
    layout->SetMaxHeight(bh - 2 * pad_y);
    *result = layout;
    return D2D1::RectF(bx, by, bx + bw, by + bh);
}

} // namespace pulse::ui
