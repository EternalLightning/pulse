// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/windows_compat.h"
#include "system_ctx_preview_drag.h"
#include "app_state.h"
#include "app_runtime.h"
#include "system_ctx_model.h"

namespace pulse {

bool SysCtxPreviewDragMove(AppState& s, int x, int y, WPARAM buttons) {
    (void)x;
    if (!s.settings.SystemCtxLoaded()) return false;
    auto& model = s.settings.SystemCtx();
    if (model.PreviewDragItem() < 0) return false;
    if (!(buttons & MK_LBUTTON)) return SysCtxPreviewDragCancel(s);
    if (model.PreviewDragMove(static_cast<float>(y))) InvalidateRect(s.hwnd, nullptr, FALSE);
    if (model.PreviewDragLive()) SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
    return true;
}

bool SysCtxPreviewDragEnd(AppState& s, int x, int y) {
    if (!s.settings.SystemCtxLoaded()) return false;
    auto& model = s.settings.SystemCtx();
    const int item = model.PreviewDragItem();
    if (item < 0) return false;
    if (model.PreviewDragLive()) {
        // The release coordinates can be newer than the last mouse-move message.
        model.PreviewDragMove(static_cast<float>(y));
        const auto vm = BuildVm(s, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                       static_cast<float>(s.compositor.Height()));
        const int zone = s.renderer.SysCtxDropZone(vm, bounds, static_cast<float>(x), static_cast<float>(y));
        model.EndPreviewDrag();
        if (zone != ui::kSysCtxZoneNone) model.DropPreview(item, zone);
    } else {
        model.EndPreviewDrag();
        model.SelectPreview(item);
    }
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool SysCtxPreviewDragCancel(AppState& s) {
    if (!s.settings.SystemCtxLoaded()) return false;
    auto& model = s.settings.SystemCtx();
    if (model.PreviewDragItem() < 0) return false;
    model.EndPreviewDrag();
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse
