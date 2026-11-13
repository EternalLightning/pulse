// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/dialog_lifecycle.h"
#include "settings_value_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/FluentTokens.h"
#include "../ui/dialog_style.h"
#include <commctrl.h>
#include <algorithm>
#include <uxtheme.h>
#include <initializer_list>
#include <utility>

namespace pulse::app {
namespace {
constexpr wchar_t kClass[] = L"PulseSettingsValueDialog";
namespace ds = ui::dialog_style;
constexpr int kContentHeight = 104;
struct DialogState {
    std::wstring hint, initial, value, confirm_label;
    size_t max_length = 32;
    HWND edit = nullptr;
    HWND modal_owner = nullptr;
    bool accepted = false;
    UINT dpi = 96;
    HFONT font = nullptr, title_font = nullptr, icon_font = nullptr, close_font = nullptr;
    bool dark = false;
    bool high_contrast = false, native_caption = false;
    D2D1_COLOR_F accent{};
    ui::Theme theme{};
    HBRUSH background = nullptr;
    HBRUSH input = nullptr;
};

COLORREF Color(D2D1_COLOR_F color) {
    return RGB(static_cast<BYTE>(color.r * 255.0f), static_cast<BYTE>(color.g * 255.0f),
               static_cast<BYTE>(color.b * 255.0f));
}
D2D1_COLOR_F RgbColor(COLORREF color) {
    return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
}
COLORREF Color(D2D1_COLOR_F color, COLORREF background) {
    return Color(ui::BlendOver(color, RgbColor(background)));
}

COLORREF Background(const DialogState& s) { return Color(ui::BlendOver(s.theme.surface_sheet, s.theme.bg)); }
COLORREF Footer(const DialogState& s) { return Color(s.theme.surface_title, Background(s)); }
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

void LayoutDialog(HWND hwnd, DialogState& state) {
    const auto px = [&state](int dip) { return MulDiv(dip, static_cast<int>(state.dpi), 96); };
    LOGFONTW metrics{};
    wcscpy_s(metrics.lfFaceName, L"Segoe UI");
    metrics.lfHeight = -px(14);
    const HFONT font = CreateFontIndirectW(&metrics);
    wcscpy_s(metrics.lfFaceName, L"Microsoft YaHei UI");
    metrics.lfWeight = FW_BOLD;
    const HFONT title = CreateFontIndirectW(&metrics);
    metrics.lfWeight = FW_NORMAL;
    metrics.lfHeight = -px(static_cast<int>(ds::kIconSize));
    wcscpy_s(metrics.lfFaceName, L"Segoe Fluent Icons");
    HFONT icon = CreateFontIndirectW(&metrics);
    HDC font_dc = GetDC(hwnd);
    const auto previous_icon = SelectObject(font_dc, icon);
    wchar_t actual_family[LF_FACESIZE]{};
    GetTextFaceW(font_dc, ARRAYSIZE(actual_family), actual_family);
    SelectObject(font_dc, previous_icon);
    ReleaseDC(hwnd, font_dc);
    if (lstrcmpiW(actual_family, metrics.lfFaceName) != 0) {
        if (icon) DeleteObject(icon);
        wcscpy_s(metrics.lfFaceName, L"Segoe MDL2 Assets");
        icon = CreateFontIndirectW(&metrics);
    }
    metrics.lfHeight = -px(12);
    const HFONT close = CreateFontIndirectW(&metrics);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.icon_font) DeleteObject(state.icon_font);
    if (state.close_font) DeleteObject(state.close_font);
    state.font = font; state.title_font = title; state.icon_font = icon; state.close_font = close;
    if (GetDlgItem(hwnd, 101)) SendMessageW(GetDlgItem(hwnd, 101), WM_SETFONT, reinterpret_cast<WPARAM>(close), TRUE);
    const int caption = state.native_caption ? 0 : static_cast<int>(ds::kTitleHeight);
    const int margin = static_cast<int>(ds::kMargin);
    MoveWindow(GetDlgItem(hwnd, 100), px(margin), px(caption + 20), px(360 - margin * 2), px(24), TRUE);
    MoveWindow(state.edit, px(state.native_caption ? margin : margin + 8),
        px(caption + (state.native_caption ? 52 : 57)), px(state.native_caption ? 320 : 304),
        px(state.native_caption ? static_cast<int>(ds::kControlHeight) : 22), TRUE);
    SendMessageW(state.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(2), px(2)));
    HDC dc = GetDC(hwnd);
    const auto old = SelectObject(dc, font);
    const auto button_width = [&](int id) {
        wchar_t label[128]{}; GetWindowTextW(GetDlgItem(hwnd, id), label, ARRAYSIZE(label));
        SIZE size{}; GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
        return std::max(px(80), static_cast<int>(size.cx) + px(32));
    };
    const int primary = button_width(IDOK), cancel = button_width(IDCANCEL);
    SelectObject(dc, old); ReleaseDC(hwnd, dc);
    MoveWindow(GetDlgItem(hwnd, IDOK), px(360 - margin) - primary, px(caption + kContentHeight + static_cast<int>((ds::kFooterHeight - ds::kControlHeight) / 2)),
        primary, px(static_cast<int>(ds::kControlHeight)), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), px(360 - margin) - primary - px(static_cast<int>(ds::kGap)) - cancel,
        px(caption + kContentHeight + static_cast<int>((ds::kFooterHeight - ds::kControlHeight) / 2)), cancel, px(static_cast<int>(ds::kControlHeight)), TRUE);
    if (!state.native_caption) {
        RECT client{};
        GetClientRect(hwnd, &client);
        MoveWindow(GetDlgItem(hwnd, 101), client.right - px(static_cast<int>(ds::kCloseWidth)), 0,
            px(static_cast<int>(ds::kCloseWidth)), px(caption), TRUE);
        HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, px(16), px(16));
        if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
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
        const auto px = [state](int dip) { return MulDiv(dip, static_cast<int>(state->dpi), 96); };
        CreateWindowExW(0, L"STATIC", state->hint.c_str(), WS_CHILD | WS_VISIBLE,
                        px(18), px(16), px(324), px(34), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)), nullptr, nullptr);
        state->edit = CreateWindowExW(state->native_caption ? WS_EX_CLIENTEDGE : 0, L"EDIT", state->initial.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (state->native_caption ? WS_BORDER : 0),
            px(18), px(54), px(324), px(27), hwnd, nullptr, nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, state->max_length, 0);
        const std::wstring apply_text = state->confirm_label.empty()
            ? l10n::Get(l10n::StringId::Apply) : state->confirm_label;
        const DWORD button_style = state->native_caption ? BS_PUSHBUTTON : BS_OWNERDRAW;
        CreateWindowExW(0, L"BUTTON", apply_text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                        (state->native_caption ? BS_DEFPUSHBUTTON : button_style),
                        px(180), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", l10n::Get(l10n::StringId::Cancel).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | button_style,
                        px(264), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);
        if (!state->native_caption) SetWindowTheme(state->edit, L"", L"");
        ui::UpdateWindowTheme(hwnd, state->dark && !state->native_caption);
        if (!state->native_caption) {
            CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                px(324), 0, px(36), px(36), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(101)), nullptr, nullptr);
        }
        if (!state->native_caption) {
            for (int id : {IDOK, IDCANCEL, 101}) SetWindowSubclass(GetDlgItem(hwnd, id), ButtonProc, 1, 0);
        }
        LayoutDialog(hwnd, *state);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_NCHITTEST && !state->native_caption) {
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int caption = MulDiv(static_cast<int>(ds::kTitleHeight), static_cast<int>(state->dpi), 96);
        const int close_width = MulDiv(static_cast<int>(ds::kCloseWidth), static_cast<int>(state->dpi), 96);
        if (point.y >= 0 && point.y < caption && point.x >= 0 && point.x < client.right - close_width)
            return HTCAPTION;
        return HTCLIENT;
    }
    if ((msg == WM_PAINT || msg == WM_PRINTCLIENT) && !state->native_caption) {
        PAINTSTRUCT paint{};
        HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, state->background);
        const auto px = [state](int dip) { return MulDiv(dip, static_cast<int>(state->dpi), 96); };
        RECT footer{0, px(static_cast<int>(ds::kTitleHeight) + kContentHeight), client.right, client.bottom};
        HBRUSH footer_brush = CreateSolidBrush(Footer(*state));
        FillRect(dc, &footer, footer_brush);
        DeleteObject(footer_brush);
        HBRUSH input_brush = CreateSolidBrush(Input(*state));
        HPEN input_pen = CreatePen(PS_SOLID, 1, Color(state->high_contrast ? state->theme.stroke_card : state->dark
            ? ui::HexColor(0xFFFFFF, 0.08f) : ui::HexColor(0x000000, 13.0f / 255.0f), Input(*state)));
        HGDIOBJ old_brush = SelectObject(dc, input_brush), old_pen = SelectObject(dc, input_pen);
        RoundRect(dc, px(20), px(96), px(340), px(128), px(16), px(16));
        SelectObject(dc, old_pen); SelectObject(dc, old_brush);
        DeleteObject(input_pen); DeleteObject(input_brush);
        HBRUSH underline = CreateSolidBrush(Color(GetFocus() == state->edit
            ? state->theme.accent : state->theme.stroke_input_bottom, Input(*state)));
        const RECT input_bottom{px(28), px(126), px(332), px(128)};
        FillRect(dc, &input_bottom, underline);
        DeleteObject(underline);
        RECT caption = client;
        caption.bottom = px(static_cast<int>(ds::kTitleHeight));
        HBRUSH brush = CreateSolidBrush(Footer(*state));
        FillRect(dc, &caption, brush);
        DeleteObject(brush);
        caption.left = px(48);
        caption.right -= px(static_cast<int>(ds::kCloseWidth));
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Color(state->theme.text, Background(*state)));
        HGDIOBJ old = SelectObject(dc, state->icon_font);
        RECT icon{px(20), px(12), px(40), px(32)};
        SetTextColor(dc, Color(state->high_contrast ? state->theme.text : state->theme.accent_ink, Footer(*state)));
        DrawTextW(dc, L"\xE713", -1, &icon, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        SetTextColor(dc, Color(state->theme.text, Footer(*state)));
        SelectObject(dc, state->title_font);
        DrawTextW(dc, title, -1, &caption, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
        if (msg == WM_PAINT) EndPaint(hwnd, &paint);
        return 0;
    }
    if (msg == DM_GETDEFID) return MAKELRESULT(IDOK, DC_HASDEFID);
    if (msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE) {
        state->high_contrast = ui::IsHighContrast();
        state->theme = state->high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(state->dark, state->accent);
        if (state->background) DeleteObject(state->background);
        if (state->input) DeleteObject(state->input);
        state->background = CreateSolidBrush(Background(*state));
        state->input = CreateSolidBrush(Input(*state));
        ui::UpdateWindowTheme(hwnd, state->dark && !state->high_contrast);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        state->dpi = HIWORD(wp);
        const auto* rect = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutDialog(hwnd, *state);
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, state->background);
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        const HDC dc = reinterpret_cast<HDC>(wp);
        const bool edit = msg == WM_CTLCOLOREDIT;
        SetTextColor(dc, Color(state->theme.text, edit ? Input(*state) : Background(*state)));
        SetBkColor(dc, edit ? Input(*state) : Background(*state));
        return reinterpret_cast<LRESULT>(edit ? state->input : state->background);
    }
    if (msg == WM_DRAWITEM && !state->native_caption) {
        const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw.CtlType != ODT_BUTTON) return FALSE;
        const bool primary = draw.CtlID == IDOK;
        const bool close = draw.CtlID == 101;
        const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
        const bool hover = GetPropW(draw.hwndItem, L"Pulse.ButtonHover") != nullptr;
        const auto card = ui::BlendOver(state->theme.surface_card, RgbColor(Footer(*state)));
        const auto fill = ui::BlendOver(close ? (pressed || hover ? (state->high_contrast ? state->theme.fill_hover : state->theme.danger) : state->theme.surface_title) :
            primary ? (pressed ? state->theme.accent_pressed : hover ? state->theme.accent_hover : state->theme.accent) :
            pressed ? ui::BlendOver(state->theme.fill_pressed, card) :
            hover ? ui::BlendOver(state->theme.fill_hover, card) : card, RgbColor(Footer(*state)));
        HBRUSH background = CreateSolidBrush(Footer(*state));
        FillRect(draw.hDC, &draw.rcItem, background);
        DeleteObject(background);
        HBRUSH brush = CreateSolidBrush(Color(fill));
        HPEN border = CreatePen(PS_SOLID, 1, Color(primary || close ? fill : state->theme.stroke_card, Color(fill)));
        const auto previous_brush = SelectObject(draw.hDC, brush), previous_pen = SelectObject(draw.hDC, border);
        const int radius = MulDiv(16, static_cast<int>(state->dpi), 96);
        RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom, radius, radius);
        SelectObject(draw.hDC, previous_pen); SelectObject(draw.hDC, previous_brush);
        DeleteObject(border); DeleteObject(brush);
        SetBkMode(draw.hDC, TRANSPARENT);
        const bool highlighted = state->high_contrast && (primary || pressed || hover);
        SetTextColor(draw.hDC, highlighted ? GetSysColor(COLOR_HIGHLIGHTTEXT) :
            Color(primary || (close && (pressed || hover)) ? ui::AutoAccentText(fill) : state->theme.text, Color(fill)));
        HGDIOBJ old = SelectObject(draw.hDC, close ? state->close_font : state->font);
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text, ARRAYSIZE(text));
        if (close) wcscpy_s(text, L"\xE8BB");
        RECT text_rect = draw.rcItem;
        DrawTextW(draw.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (draw.itemState & ODS_FOCUS) {
            const int inset = MulDiv(3, static_cast<int>(state->dpi), 96);
            InflateRect(&text_rect, -inset, -inset);
            DrawFocusRect(draw.hDC, &text_rect);
        }
        if (old) SelectObject(draw.hDC, old);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        if (reinterpret_cast<HWND>(lp) == state->edit &&
            (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            const RECT frame{MulDiv(20, static_cast<int>(state->dpi), 96), MulDiv(96, static_cast<int>(state->dpi), 96),
                MulDiv(340, static_cast<int>(state->dpi), 96), MulDiv(128, static_cast<int>(state->dpi), 96)};
            InvalidateRect(hwnd, &frame, FALSE);
            return 0;
        }
        if (LOWORD(wp) == IDOK) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring value(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, value.data(), length + 1);
            value.resize(static_cast<size_t>(length));
            state->value = std::move(value);
            state->accepted = true;
            ui::DestroyDialogWithFade(hwnd, state->modal_owner);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == 101) {
            ui::DestroyDialogWithFade(hwnd, state->modal_owner);
            return 0;
        }
    }
    if (msg == WM_CLOSE) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    if (msg == WM_NCDESTROY) {
        if (state->title_font) DeleteObject(state->title_font);
        if (state->icon_font) DeleteObject(state->icon_font);
        if (state->close_font) DeleteObject(state->close_font);
        if (state->font) DeleteObject(state->font);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool PromptSettingsValue(HWND owner, const std::wstring& title,
                         const std::wstring& hint, const std::wstring& initial,
                         std::wstring& result, size_t max_length,
                         const std::wstring& confirm_label, bool dark, D2D1_COLOR_F accent) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    DialogState state{hint, initial};
    state.dark = dark;
    state.high_contrast = ui::IsHighContrast();
    state.native_caption = state.high_contrast;
    state.accent = accent;
    state.theme = state.high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark, accent);
    state.background = CreateSolidBrush(state.high_contrast ? GetSysColor(COLOR_WINDOW) :
        Background(state));
    state.input = CreateSolidBrush(state.high_contrast ? GetSysColor(COLOR_WINDOW) :
        Input(state));
    struct Brushes {
        DialogState& state;
        ~Brushes() { DeleteObject(state.background); DeleteObject(state.input); }
    } brushes{state};
    state.max_length = max_length;
    state.confirm_label = confirm_label;
    state.dpi = compat::WindowDpi(owner);
    const DWORD style = (state.high_contrast ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU : WS_POPUP) | WS_CLIPCHILDREN;
    const DWORD ex_style = WS_EX_CONTROLPARENT | (state.high_contrast ? WS_EX_DLGMODALFRAME : 0);
    RECT bounds{0, 0, MulDiv(360, static_cast<int>(state.dpi), 96),
        MulDiv(kContentHeight + static_cast<int>(ds::kFooterHeight) +
            (state.native_caption ? 0 : static_cast<int>(ds::kTitleHeight)), static_cast<int>(state.dpi), 96)};
    if (state.high_contrast) {
        using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const auto adjust = reinterpret_cast<AdjustForDpi>(compat::UserApi("AdjustWindowRectExForDpi"));
        if (adjust) adjust(&bounds, style, FALSE, ex_style, state.dpi);
        else AdjustWindowRectEx(&bounds, style, FALSE, ex_style);
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    RECT parent{};
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        monitor.rcWork = parent;
    const int x = std::clamp(parent.left + ((parent.right - parent.left) - width) / 2,
        monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    const int y = std::clamp(parent.top + ((parent.bottom - parent.top) - height) / 2,
        monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    HWND dialog = CreateWindowExW(ex_style,
        kClass, title.c_str(), style,
        x, y, width, height, owner, nullptr, wc.hInstance, &state);
    if (!dialog) return false;
    ui::OwnerDimScope owner_dim(owner);
    state.modal_owner = IsWindowEnabled(owner) ? owner : nullptr;
    if (state.modal_owner) EnableWindow(owner, FALSE);
    ui::ShowDialogWithFade(dialog);
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
