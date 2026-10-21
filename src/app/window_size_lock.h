#pragma once

#include <windows.h>

namespace pulse::app {

class WindowSizeLock {
public:
    void Sync(HWND hwnd, bool enabled) noexcept {
        if (!enabled) {
            normal_size_ = {};
            return;
        }
        if (normal_size_.cx <= 0 || normal_size_.cy <= 0) {
            RECT rect{};
            if (!IsZoomed(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &rect)) {
                normal_size_ = {rect.right - rect.left, rect.bottom - rect.top};
                return;
            }
            WINDOWPLACEMENT placement{sizeof(placement)};
            if (!GetWindowPlacement(hwnd, &placement)) return;
            rect = placement.rcNormalPosition;
            normal_size_ = {rect.right - rect.left, rect.bottom - rect.top};
        }
    }

    void Constrain(HWND hwnd, bool enabled, WINDOWPOS& position) noexcept {
        Sync(hwnd, enabled);
        if (!enabled || normal_size_.cx <= 0 || normal_size_.cy <= 0) return;
        // Preserve the normal size while Windows maximizes or minimizes the window.
        if ((position.flags & SWP_NOSIZE) || IsZoomed(hwnd) || IsIconic(hwnd)) return;
        position.cx = normal_size_.cx;
        position.cy = normal_size_.cy;
    }

private:
    SIZE normal_size_{};
};

} // namespace pulse::app
