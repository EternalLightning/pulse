#pragma once
#include <windows.h>
#include "ui_compositor.h"

namespace pulse::ui {
COLORREF ChildEditTextColor(bool dark);
COLORREF ChildEditBackColor(bool dark);
HBRUSH ChildEditBackBrush(HBRUSH themed);
inline D2D1_COLOR_F ChildEditColor(COLORREF color) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f);
}
// Initial presentation uses the same native fallback as later text updates.
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd);
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, LRESULT& result);
LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background,
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
// EDIT owns native editing and IME. Its LumaText bitmap is a child surface,
// clipped and moved by the parent rather than an independently owned popup.
inline HWND CreateChildEdit(HWND parent, const wchar_t* text = L"", DWORD edit_style = 0) {
    return CreateWindowExW(WS_EX_LAYERED, L"EDIT", text,
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | edit_style,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
}
