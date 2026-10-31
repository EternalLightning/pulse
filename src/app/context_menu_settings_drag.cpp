// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "context_menu_settings_drag.h"
#include "app_internal.h"

namespace pulse {

bool PulseMenuPreviewDragCancel(AppState& state) {
    if (state.settings.menu_drag_item() < 0) return false;
    state.settings.EndMenuDrag();
    if (GetCapture() == state.hwnd) ReleaseCapture();
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

bool PulseMenuPreviewDragMove(AppState& state, int x, int y, WPARAM buttons) {
    if (state.settings.menu_drag_item() < 0) return false;
    if (!(buttons & MK_LBUTTON)) return PulseMenuPreviewDragCancel(state);
    const bool horizontal = state.settings.ContextTab() == 2;
    if (state.settings.MenuDragMove(static_cast<float>(horizontal ? x : y)))
        InvalidateRect(state.hwnd, nullptr, FALSE);
    if (state.settings.menu_drag_live())
        SetCursor(LoadCursorW(nullptr, horizontal ? IDC_SIZEWE : IDC_SIZENS));
    return true;
}

bool PulseMenuPreviewDragEnd(AppState& state, int x, int y) {
    if (state.settings.menu_drag_item() < 0) return false;
    model::BuiltinMenuOrder order;
    if (state.settings.menu_drag_live()) {
        state.settings.MenuDragMove(static_cast<float>(state.settings.ContextTab() == 2 ? x : y));
        const auto vm = BuildVm(state, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                                      static_cast<float>(state.compositor.Height()));
        order = state.renderer.SettingsMenuDropOrder(vm, bounds, static_cast<float>(x), static_cast<float>(y));
    }
    state.settings.EndMenuDrag();
    if (!order.empty()) state.settings.SetMenuOrder(order);
    if (GetCapture() == state.hwnd) ReleaseCapture();
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

} // namespace pulse
