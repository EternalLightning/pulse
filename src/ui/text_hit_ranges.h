// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <dwrite.h>
#include <vector>

namespace pulse::ui {

inline std::vector<DWRITE_HIT_TEST_METRICS> TextHitRanges(IDWriteTextLayout* layout,
    UINT32 start, UINT32 length, float origin_x, float origin_y) {
    if (!layout || !length) return {};
    DWRITE_HIT_TEST_METRICS hit_metrics[96];
    UINT32 count = 0;
    const HRESULT first = layout->HitTestTextRange(start, length, origin_x, origin_y,
        hit_metrics, ARRAYSIZE(hit_metrics), &count);
    if (SUCCEEDED(first)) return {hit_metrics, hit_metrics + count};
    if (first != E_NOT_SUFFICIENT_BUFFER || !count) return {};
    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
    if (FAILED(layout->HitTestTextRange(start, length, origin_x, origin_y,
        ranges.data(), static_cast<UINT32>(ranges.size()), &count))) return {};
    ranges.resize(count);
    return ranges;
}

} // namespace pulse::ui
