// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_input.h"
#include "cloud_commands.h"
#include <windowsx.h>
#include "../ui/preview_grab_cursor.h"

namespace pulse {
bool HandleDetailsPreviewPointer(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!s) return false;
    const float x = static_cast<float>(GET_X_LPARAM(lParam));
    const float y = static_cast<float>(GET_Y_LPARAM(lParam));
    if (msg == WM_LBUTTONDOWN && s->showDetailsPanel) {
        const auto vm = BuildVm(*s, false);
        const auto rect = D2D1::RectF(0, 0, static_cast<float>(s->compositor.Width()),
                                    static_cast<float>(s->compositor.Height()));
        const auto hit = s->renderer.HitTest(vm, rect, x, y);
        if (hit.region == ui::HitTestResult::DetailsCloudDownload) {
            HandleCloudCommand(*s, vm.details.cloud_download_percent >= 0
                ? app::CmdCloudCancelDownload : app::CmdCloudDownload);
            return true;
        }
        if (hit.region != ui::HitTestResult::DetailsPreview)
            return false;
        if (s->renderer.BeginDetailsPreviewPan(x, y)) {
            s->dragPending = false;
            s->detailsPreviewPanning = true;
            SetCapture(hwnd);
            SetCursor(ui::PreviewGrabCursor(s->renderer.DetailsPreviewDragging()));
        }
        return true;
    }
    if (!s->detailsPreviewPanning) return false;
    if (!s->renderer.DetailsPreviewPointerActive() || msg == WM_LBUTTONUP || (msg == WM_MOUSEMOVE && !(wParam & MK_LBUTTON))) {
        s->detailsPreviewPanning = false;
        s->renderer.EndDetailsPreviewPan();
        SetCursor(ui::PreviewGrabCursor(false));
        if (GetCapture() == hwnd) ReleaseCapture();
    } else if (msg == WM_MOUSEMOVE) {
        s->renderer.MoveDetailsPreviewPan(x, y);
        SetCursor(ui::PreviewGrabCursor(s->renderer.DetailsPreviewDragging()));
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}
} // namespace pulse
