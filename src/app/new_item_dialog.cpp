// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/dialog_lifecycle.h"
#include "../ui/dialog_style.h"
#include "../ui/FluentTokens.h"
#include "new_item_dialog.h"
#include "batch_rename.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <utility>

namespace pulse::app {
namespace {
constexpr wchar_t kClass[] = L"PulseNewItemDialog";
namespace ds = ui::dialog_style;
constexpr int kWidth = 350, kFooterTop = 160;
constexpr int kHeight = kFooterTop + static_cast<int>(ds::kFooterHeight), kClose = 101;

struct DialogState {
    std::wstring title, value;
    HWND edit = nullptr, create = nullptr;
    HWND modal_owner = nullptr;
    HFONT title_font = nullptr, text_font = nullptr, icon_font = nullptr, close_font = nullptr;
    bool high_contrast = false;
    ui::Theme theme{};
    HBRUSH edit_brush = nullptr;
    bool dark = false, accepted = false, invalid = false;
    COLORREF accent = RGB(80, 180, 220);
    UINT dpi = 96;
    size_t max_length = 255;
};

int Px(const DialogState& state, int dip) {
    return MulDiv(dip, static_cast<int>(state.dpi), 96);
}

COLORREF Color(D2D1_COLOR_F color) {
    return RGB(static_cast<BYTE>(color.r * 255), static_cast<BYTE>(color.g * 255),
               static_cast<BYTE>(color.b * 255));
}
D2D1_COLOR_F RgbColor(COLORREF color) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
}
COLORREF Color(D2D1_COLOR_F color, COLORREF background) {
    return Color(ui::BlendOver(color, RgbColor(background)));
}

COLORREF Background(const DialogState& s) { return Color(ui::BlendOver(s.theme.surface_sheet, s.theme.bg)); }
COLORREF Footer(const DialogState& s) { return Color(s.theme.surface_title, Background(s)); }
COLORREF Text(const DialogState& s) { return Color(s.theme.text, Background(s)); }
COLORREF Input(const DialogState& s) { return Color(s.theme.fill_input_focus, Background(s)); }

LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && !GetPropW(hwnd, L"Pulse.ButtonHover")) {
        SetPropW(hwnd, L"Pulse.ButtonHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"Pulse.ButtonHover");
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void Layout(HWND hwnd, DialogState& s) {
    LOGFONTW font{};
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    font.lfHeight = -Px(s, 14);
    const HFONT text = CreateFontIndirectW(&font);
    wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    font.lfWeight = FW_BOLD;
    const HFONT title = CreateFontIndirectW(&font);
    font.lfWeight = FW_NORMAL;
    font.lfHeight = -Px(s, static_cast<int>(ds::kIconSize));
    wcscpy_s(font.lfFaceName, L"Segoe Fluent Icons");
    HFONT icon = CreateFontIndirectW(&font);
    HDC font_dc = GetDC(hwnd);
    const auto previous_icon = SelectObject(font_dc, icon);
    wchar_t actual_family[LF_FACESIZE]{};
    GetTextFaceW(font_dc, ARRAYSIZE(actual_family), actual_family);
    SelectObject(font_dc, previous_icon);
    ReleaseDC(hwnd, font_dc);
    if (lstrcmpiW(actual_family, font.lfFaceName) != 0) {
        if (icon) DeleteObject(icon);
        wcscpy_s(font.lfFaceName, L"Segoe MDL2 Assets");
        icon = CreateFontIndirectW(&font);
    }
    font.lfHeight = -Px(s, 12);
    const HFONT close = CreateFontIndirectW(&font);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(text), TRUE);
    if (s.text_font) DeleteObject(s.text_font);
    if (s.title_font) DeleteObject(s.title_font);
    if (s.icon_font) DeleteObject(s.icon_font);
    if (s.close_font) DeleteObject(s.close_font);
    s.text_font = text; s.title_font = title; s.icon_font = icon; s.close_font = close;
    if (GetDlgItem(hwnd, kClose)) SendMessageW(GetDlgItem(hwnd, kClose), WM_SETFONT, reinterpret_cast<WPARAM>(close), TRUE);
    MoveWindow(s.edit, Px(s, 28), Px(s, 105), Px(s, kWidth - 56), Px(s, 22), TRUE);
    SendMessageW(s.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Px(s, 2), Px(s, 2)));
    HDC dc = GetDC(hwnd);
    const auto old = SelectObject(dc, s.text_font);
    const auto button_width = [&](int id) {
        wchar_t label[128]{}; GetWindowTextW(GetDlgItem(hwnd, id), label, ARRAYSIZE(label));
        SIZE size{}; GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
        return std::max(Px(s, 80), static_cast<int>(size.cx) + Px(s, 32));
    };
    const int primary = button_width(IDOK), cancel = button_width(IDCANCEL);
    SelectObject(dc, old); ReleaseDC(hwnd, dc);
    const int right = Px(s, kWidth - static_cast<int>(ds::kMargin));
    const int y = Px(s, kFooterTop + 16), height = Px(s, static_cast<int>(ds::kControlHeight));
    MoveWindow(GetDlgItem(hwnd, IDOK), right - primary, y, primary, height, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), right - primary - Px(s, static_cast<int>(ds::kGap)) - cancel,
        y, cancel, height, TRUE);
    MoveWindow(GetDlgItem(hwnd, kClose), Px(s, kWidth - static_cast<int>(ds::kCloseWidth)), 0,
        Px(s, static_cast<int>(ds::kCloseWidth)), Px(s, static_cast<int>(ds::kTitleHeight)), TRUE);
    RECT bounds{}; GetClientRect(hwnd, &bounds);
    HRGN region = CreateRoundRectRgn(0, 0, bounds.right + 1, bounds.bottom + 1, Px(s, 16), Px(s, 16));
    if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
    InvalidateRect(hwnd, nullptr, FALSE);
}

RECT ErrorBounds(const DialogState& state) {
    return {Px(state, 20), Px(state, 138), Px(state, 330), Px(state, 156)};
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void PaintDialog(HWND hwnd, const DialogState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    Fill(dc, client, Background(state));
    Fill(dc, {0, 0, client.right, Px(state, static_cast<int>(ds::kTitleHeight))}, Footer(state));
    Fill(dc, {0, Px(state, 160), client.right, client.bottom}, Footer(state));
    Fill(dc, {0, Px(state, 160), client.right, Px(state, 161)},
         Color(state.theme.stroke_divider, Footer(state)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Text(state));
    HGDIOBJ old_font = SelectObject(dc, state.title_font);
    SelectObject(dc, state.icon_font);
    RECT icon{Px(state, 20), Px(state, 12), Px(state, 40), Px(state, 32)};
    SetTextColor(dc, Color(state.high_contrast ? state.theme.text : state.theme.accent_ink, Footer(state)));
    DrawTextW(dc, L"\xE710", -1, &icon, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SetTextColor(dc, Text(state));
    SelectObject(dc, state.title_font);
    RECT title{Px(state, 48), 0, Px(state, kWidth - 46), Px(state, 44)};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, state.text_font);
    const auto& prompt = l10n::Get(l10n::StringId::CreateItemNamePrompt);
    RECT hint{Px(state, 20), Px(state, 64), Px(state, 330), Px(state, 88)};
    DrawTextW(dc, prompt.c_str(), -1, &hint, DT_SINGLELINE | DT_VCENTER);
    const auto outline = state.high_contrast ? state.theme.stroke_card : state.dark
        ? ui::HexColor(0xFFFFFF, 0.08f) : ui::HexColor(0x000000, 13.0f / 255.0f);
    const COLORREF border = Color(outline, Input(state));
    HBRUSH input_brush = CreateSolidBrush(Input(state));
    HPEN input_pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, input_brush);
    HGDIOBJ old_pen = SelectObject(dc, input_pen);
    RoundRect(dc, Px(state, 20), Px(state, 100), Px(state, 330), Px(state, 132),
              Px(state, 16), Px(state, 16));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(input_pen);
    DeleteObject(input_brush);
    Fill(dc, {Px(state, 28), Px(state, 130), Px(state, 322), Px(state, 132)},
        Color(GetFocus() == state.edit ? state.theme.accent : state.theme.stroke_input_bottom, Input(state)));
    if (state.invalid) {
        SetTextColor(dc, state.high_contrast ? Text(state) : Color(state.theme.danger, Background(state)));
        const auto& error = l10n::Get(l10n::StringId::InvalidName);
        RECT error_rect = ErrorBounds(state);
        DrawTextW(dc, error.c_str(), -1, &error_rect, DT_SINGLELINE | DT_VCENTER);
    }
    HPEN border_pen = CreatePen(PS_SOLID, 1,
        Color(state.theme.stroke_card, Background(state)));
    old_pen = SelectObject(dc, border_pen);
    old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, 0, 0, client.right - 1, client.bottom - 1,
              Px(state, 16), Px(state, 16));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border_pen);
    SelectObject(dc, old_font);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(hwnd, &paint);
}

void PaintButton(const DialogState& state, const DRAWITEMSTRUCT& item) {
    const bool create = item.CtlID == IDOK;
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool close = item.CtlID == kClose;
    const bool hover = GetPropW(item.hwndItem, L"Pulse.ButtonHover") != nullptr;
    const auto card = ui::BlendOver(state.theme.surface_card, RgbColor(Footer(state)));
    const auto fill_color = ui::BlendOver(create && enabled ? (pressed ? state.theme.accent_pressed : hover ? state.theme.accent_hover : state.theme.accent)
        : close ? (pressed || hover ? (state.high_contrast ? state.theme.fill_hover : state.theme.danger) : state.theme.surface_title)
        : pressed ? ui::BlendOver(state.theme.fill_pressed, card)
        : hover ? ui::BlendOver(state.theme.fill_hover, card) : card, RgbColor(Footer(state)));
    const COLORREF fill = Color(fill_color);
    Fill(item.hDC, item.rcItem, Footer(state));
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, create || close ? fill : Color(state.theme.stroke_card, fill));
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top,
              item.rcItem.right, item.rcItem.bottom, Px(state, 16), Px(state, 16));
    SelectObject(item.hDC, old_pen);
    SelectObject(item.hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    const bool highlighted = state.high_contrast && (create || hover || pressed);
    SetTextColor(item.hDC, !enabled ? Color(state.theme.text_disabled, fill)
        : highlighted ? GetSysColor(COLOR_HIGHLIGHTTEXT)
        : create || (close && (hover || pressed)) ? Color(ui::AutoAccentText(fill_color)) : Color(state.theme.text, fill));
    HGDIOBJ old_font = SelectObject(item.hDC, close ? state.close_font : state.text_font);
    const std::wstring label = close ? L"\xE8BB" : l10n::Get(create ? l10n::StringId::CreateItemAction : l10n::StringId::Cancel);
    RECT text_rect = item.rcItem;
    DrawTextW(item.hDC, label.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(item.hDC, old_font);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Px(state, 3), -Px(state, 3));
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK EditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp,
                          UINT_PTR, DWORD_PTR reference) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        PostMessageW(GetParent(edit), WM_COMMAND, wp == VK_RETURN ? IDOK : IDCANCEL, 0);
        return 0;
    }
    const LRESULT result = DefSubclassProc(edit, msg, wp, lp);
    if ((msg == WM_PAINT || msg == WM_PRINTCLIENT) && GetWindowTextLengthW(edit) == 0) {
        HDC dc = msg == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wp) : GetDC(edit);
        if (dc) {
            RECT rect{};
            GetClientRect(edit, &rect);
            rect.left += 2;
            SetBkMode(dc, TRANSPARENT);
            const auto* state = reinterpret_cast<const DialogState*>(reference);
            SetTextColor(dc, Color(state->theme.text_secondary, Input(*state)));
            HGDIOBJ old_font = SelectObject(dc,
                reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)));
            const auto& cue = l10n::Get(l10n::StringId::CreateItemNamePrompt);
            DrawTextW(dc, cue.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, old_font);
            if (msg == WM_PAINT) ReleaseDC(edit, dc);
        }
    }
    return result;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        state = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_CREATE) {
        state->edit_brush = CreateSolidBrush(Input(*state));
        state->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            Px(*state, 33), Px(*state, 108), Px(*state, 284), Px(*state, 24),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, state->max_length, 0);
        SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Px(*state, 2), Px(*state, 2)));
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->text_font), TRUE);
        SetWindowSubclass(state->edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(state));
        state->create = CreateWindowExW(0, L"BUTTON",
            l10n::Get(l10n::StringId::CreateItemAction).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            Px(*state, 24), Px(*state, 184), Px(*state, 146), Px(*state, 32),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", l10n::Get(l10n::StringId::Cancel).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            Px(*state, 179), Px(*state, 184), Px(*state, 146), Px(*state, 32),
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kClose)), nullptr, nullptr);
        for (int id : {IDOK, IDCANCEL, kClose}) SetWindowSubclass(GetDlgItem(hwnd, id), ButtonProc, 1, 0);
        Layout(hwnd, *state);
        EnableWindow(state->create, FALSE);
        SetFocus(state->edit);
        return 0;
    }
    if (msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE) {
        state->high_contrast = ui::IsHighContrast();
        const auto accent = state->accent;
        state->theme = state->high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(state->dark,
            D2D1::ColorF(GetRValue(accent) / 255.0f, GetGValue(accent) / 255.0f, GetBValue(accent) / 255.0f));
        if (state->edit_brush) DeleteObject(state->edit_brush);
        state->edit_brush = CreateSolidBrush(Input(*state));
        ui::UpdateWindowTheme(hwnd, state->dark && !state->high_contrast);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        state->dpi = HIWORD(wp);
        const auto* rect = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(hwnd, *state);
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &point);
        if (point.y >= 0 && point.y < Px(*state, static_cast<int>(ds::kTitleHeight)) &&
            point.x >= 0 && point.x < Px(*state, kWidth - static_cast<int>(ds::kCloseWidth))) return HTCAPTION;
        return HTCLIENT;
    }
    if (msg == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(lp) == state->edit) {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, Input(*state));
        SetTextColor(dc, Text(*state));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    if (msg == WM_DRAWITEM) {
        PaintButton(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    }
    if (msg == WM_PAINT) { PaintDialog(hwnd, *state); return 0; }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == 100 && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            const RECT frame{Px(*state, 20), Px(*state, 100), Px(*state, 330), Px(*state, 132)};
            InvalidateRect(hwnd, &frame, FALSE);
            return 0;
        }
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, name.data(), length + 1);
            name.resize(static_cast<size_t>(length));
            const bool valid = IsValidFileName(name);
            const bool invalid = !name.empty() && !valid;
            if (state->invalid != invalid) {
                state->invalid = invalid;
                const RECT error_rect = ErrorBounds(*state);
                InvalidateRect(hwnd, &error_rect, FALSE);
            }
            if ((IsWindowEnabled(state->create) != FALSE) != valid)
                EnableWindow(state->create, valid);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            if (!IsWindowEnabled(state->create)) return 0;
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, name.data(), length + 1);
            name.resize(static_cast<size_t>(length));
            if (!IsValidFileName(name)) return 0;
            state->value = std::move(name);
            state->accepted = true;
            ui::DestroyDialogWithFade(hwnd, state->modal_owner);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == kClose) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    }
    if (msg == WM_CLOSE) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    if (msg == WM_NCDESTROY) {
        if (state->icon_font) DeleteObject(state->icon_font);
        if (state->close_font) DeleteObject(state->close_font);
        if (state->title_font) DeleteObject(state->title_font);
        if (state->text_font) DeleteObject(state->text_font);
        if (state->edit_brush) DeleteObject(state->edit_brush);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool PromptNewItemName(HWND owner, const std::wstring& title, bool dark,
                       COLORREF accent, std::wstring& result, size_t max_length) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    DialogState state{};
    state.title = title;
    state.dark = dark;
    state.accent = accent;
    state.high_contrast = ui::IsHighContrast();
    state.theme = state.high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark,
        D2D1::ColorF(GetRValue(accent) / 255.0f, GetGValue(accent) / 255.0f, GetBValue(accent) / 255.0f));
    state.dpi = compat::WindowDpi(owner);
    state.max_length = max_length;
    const int width = Px(state, kWidth), height = Px(state, kHeight);
    RECT parent{};
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        monitor.rcWork = parent;
    const int x = std::clamp(parent.left + ((parent.right - parent.left) - width) / 2,
        monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    const int y = std::clamp(parent.top + ((parent.bottom - parent.top) - height) / 2,
        monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        kClass, title.c_str(), WS_POPUP | WS_CLIPCHILDREN, x, y, width, height,
        owner, nullptr, wc.hInstance, &state);
    if (!dialog) return false;
    HRGN round = CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(state, 16), Px(state, 16));
    if (round && !SetWindowRgn(dialog, round, TRUE)) DeleteObject(round);
    const BOOL use_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
    ui::OwnerDimScope owner_dim(owner);
    state.modal_owner = IsWindowEnabled(owner) ? owner : nullptr;
    if (state.modal_owner) EnableWindow(owner, FALSE);
    ui::ShowDialogWithFade(dialog);
    SetForegroundWindow(dialog);
    SetFocus(state.edit);
    MSG msg{};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dialog)) ui::DestroyDialogWithFade(dialog, state.modal_owner);
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    if (state.modal_owner && IsWindow(owner)) EnableWindow(owner, TRUE);
    if (state.accepted) result = std::move(state.value);
    return state.accepted;
}
} // namespace pulse::app
