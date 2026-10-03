#include "edit_host.h"
#include "FluentTokens.h"
#include <commctrl.h>
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
namespace {
constexpr wchar_t kNativeEdit[] = L"Pulse.NativeEditFallback";
bool CustomEdit(Compositor& compositor, HWND hwnd) {
    return compositor.LumaTextEnabled() && !GetPropW(hwnd, kNativeEdit);
}
bool PresentEdit(Compositor& compositor, HWND hwnd, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    if (compositor.PresentLumaEdit(hwnd, format, foreground, background)) return true;
    // Keep native EDIT input, selection and IME together if presentation fails.
    SetPropW(hwnd, kNativeEdit, reinterpret_cast<HANDLE>(1));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    KillTimer(hwnd, kEditCaretTimer);
    if (GetFocus() == hwnd) ShowCaret(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    return false;
}
}
COLORREF ChildEditTextColor(bool dark) {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOWTEXT)
                           : dark ? RGB(255, 255, 255) : RGB(26, 26, 26);
}
COLORREF ChildEditBackColor(bool dark) {
    return IsHighContrast() ? GetSysColor(COLOR_WINDOW)
                           : dark ? RGB(30, 30, 30) : RGB(255, 255, 255);
}
HBRUSH ChildEditBackBrush(HBRUSH themed) {
    return IsHighContrast() ? GetSysColorBrush(COLOR_WINDOW) : themed;
}
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd) {
    if (!hwnd || !CustomEdit(compositor, hwnd)) return false;
    HideCaret(hwnd);
    return PresentEdit(compositor, hwnd, format, foreground, background);
}
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    (void)background_brush;
    if (msg == WM_NCDESTROY) RemovePropW(hwnd, kNativeEdit);
    if (CustomEdit(compositor, hwnd) &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!CustomEdit(compositor, hwnd)) return false;
        result = compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, format);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        }
        return true;
    }
    case WM_PAINT: {
        if (!CustomEdit(compositor, hwnd)) return false;
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (CustomEdit(compositor, hwnd)) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && CustomEdit(compositor, hwnd)) {
                PresentEdit(compositor, hwnd, format,
                                             foreground, background);
            } else if (GetCapture() != hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format, D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool changes_visual = msg == WM_SETTEXT || msg == EM_SETSEL || msg == EM_REPLACESEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_CUT || msg == WM_PASTE ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_SETFONT || msg == WM_SIZE || msg == EM_SETMARGINS ||
        msg == EM_SETCUEBANNER;
    const bool custom_paint = changes_visual && CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd);
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format,
            foreground, background);
    }
    return result;
}

}
