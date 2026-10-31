// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <d2d1.h>

namespace pulse::ui {
// Adapts embedded SVG artwork for legacy render targets that cannot draw SVG.
HRESULT CreateSvgResourceBitmap(ID2D1RenderTarget* target, int resource_id,
                                D2D1_SIZE_U size, D2D1_SIZE_F viewport, ID2D1Bitmap** bitmap);
}
