// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "ui_compositor.h"
#include "../index/index_query.h"
#include <vector>

namespace pulse::ui {
inline constexpr float kNameHighlightPaddingDip = 2.0f;

struct NameMatchRange {
    UINT32 start = 0;
    UINT32 length = 0;
};

std::vector<index::Term> NameHighlightTerms(std::wstring_view filter, std::wstring_view search);
std::vector<NameMatchRange> NameMatchRanges(std::wstring_view name, const std::vector<index::Term>& terms);
std::vector<NameMatchRange> VisibleNameMatchRanges(std::wstring_view original, std::wstring_view shown,
                                                const std::vector<NameMatchRange>& ranges);
void ApplyNameHighlightPadding(IDWriteTextLayout* layout,
                               const std::vector<NameMatchRange>& ranges, float scale);
void DrawNameHighlightBackground(Compositor* compositor, IDWriteTextLayout* layout,
                                 D2D1_POINT_2F origin, const D2D1_RECT_F& clip,
                                 const std::vector<NameMatchRange>& ranges, const Theme& theme, float scale);
}
