#include "../ui/dialog_lifecycle.h"
#include "settings_value_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/FluentTokens.h"
#include <uxtheme.h>
#include <initializer_list>
#include <utility>

namespace pulse::app {
namespace {
constexpr wchar_t kClass[] = L"PulseSettingsValueDialog";
struct DialogState {
    std::wstring hint, initial, value, confirm_label;
    size_t max_length = 32;
    HWND edit = nullptr;
    HWND modal_owner = nullptr;
    bool accepted = false;
    UINT dpi = 96;
    HFONT font = nullptr;
    bool dark = false;
    bool high_contrast = false;
    ui::Theme theme{};
    HBRUSH background = nullptr;
    HBRUSH input = nullptr;
};

COLORREF Color(D2D1_COLOR_F color) {
    return RGB(static_cast<BYTE>(color.r * 255.0f), static_cast<BYTE>(color.g * 255.0f),
               static_cast<BYTE>(color.b * 255.0f));
}

void LayoutDialog(HWND hwnd, DialogState& state) {
    const auto px = [&state](int dip) { return MulDiv(dip, static_cast<int>(state.dpi), 96); };
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        HDC dc = GetDC(nullptr);
        const int system_dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        metrics.lfMessageFont.lfHeight = MulDiv(metrics.lfMessageFont.lfHeight,
            static_cast<int>(state.dpi), system_dpi);
        HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
        if (font) {
            for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            if (state.font) DeleteObject(state.font);
            state.font = font;
        }
    }
    const int caption = state.high_contrast ? 0 : 36;
    MoveWindow(GetDlgItem(hwnd, 100), px(18), px(caption + 16), px(324), px(34), TRUE);
    MoveWindow(state.edit, px(18), px(caption + 54), px(324), px(27), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDOK), px(180), px(caption + 97), px(78), px(29), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), px(264), px(caption + 97), px(78), px(29), TRUE);
    if (!state.high_contrast) {
        RECT client{};
        GetClientRect(hwnd, &client);
        MoveWindow(GetDlgItem(hwnd, 101), client.right - px(36), 0, px(36), px(36), TRUE);
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
                        px(18), px(16), px(324), px(34), hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        state->edit = CreateWindowExW(state->high_contrast ? WS_EX_CLIENTEDGE : 0, L"EDIT", state->initial.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
            px(18), px(54), px(324), px(27), hwnd, nullptr, nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, state->max_length, 0);
        const std::wstring apply_text = state->confirm_label.empty()
            ? l10n::Get(l10n::StringId::Apply) : state->confirm_label;
        const DWORD button_style = state->high_contrast ? BS_PUSHBUTTON : BS_OWNERDRAW;
        CreateWindowExW(0, L"BUTTON", apply_text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                        (state->high_contrast ? BS_DEFPUSHBUTTON : button_style),
                        px(180), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", l10n::Get(l10n::StringId::Cancel).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | button_style,
                        px(264), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        if (!state->high_contrast) SetWindowTheme(state->edit, L"", L"");
        ui::UpdateWindowTheme(hwnd, state->dark && !state->high_contrast);
        if (!state->high_contrast) {
            CreateWindowExW(0, L"BUTTON", L"×", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                px(324), 0, px(36), px(36), hwnd, reinterpret_cast<HMENU>(101), nullptr, nullptr);
        }
        LayoutDialog(hwnd, *state);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_NCHITTEST && !state->high_contrast) {
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int caption = MulDiv(36, static_cast<int>(state->dpi), 96);
        if (point.y >= 0 && point.y < caption && point.x >= 0 && point.x < client.right - caption)
            return HTCAPTION;
        return HTCLIENT;
    }
    if ((msg == WM_PAINT || msg == WM_PRINTCLIENT) && !state->high_contrast) {
        PAINTSTRUCT paint{};
        HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, state->background);
        RECT caption = client;
        caption.bottom = MulDiv(36, static_cast<int>(state->dpi), 96);
        HBRUSH brush = CreateSolidBrush(Color(state->theme.surface_title));
        FillRect(dc, &caption, brush);
        DeleteObject(brush);
        caption.left += MulDiv(18, static_cast<int>(state->dpi), 96);
        caption.right -= MulDiv(42, static_cast<int>(state->dpi), 96);
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Color(state->theme.text));
        HGDIOBJ old = state->font ? SelectObject(dc, state->font) : nullptr;
        DrawTextW(dc, title, -1, &caption, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
        if (msg == WM_PAINT) EndPaint(hwnd, &paint);
        return 0;
    }
    if (msg == DM_GETDEFID) return MAKELRESULT(IDOK, DC_HASDEFID);
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
        SetTextColor(dc, state->high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : Color(state->theme.text));
        SetBkColor(dc, state->high_contrast ? GetSysColor(COLOR_WINDOW) :
            Color(edit ? state->theme.fill_input_focus : state->theme.surface_sheet));
        return reinterpret_cast<LRESULT>(edit ? state->input : state->background);
    }
    if (msg == WM_DRAWITEM && !state->high_contrast) {
        const auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw.CtlType != ODT_BUTTON) return FALSE;
        const bool primary = draw.CtlID == IDOK;
        const bool close = draw.CtlID == 101;
        const auto fill = close ? ((draw.itemState & ODS_SELECTED) ? state->theme.danger : state->theme.surface_title) :
            primary ? ((draw.itemState & ODS_SELECTED) ? state->theme.accent_pressed :
            state->theme.accent) : state->theme.surface_card;
        HBRUSH brush = CreateSolidBrush(Color(fill));
        FillRect(draw.hDC, &draw.rcItem, brush);
        DeleteObject(brush);
        if (!close) {
            HBRUSH border = CreateSolidBrush(Color(primary ? state->theme.accent : state->theme.stroke_input_bottom));
            FrameRect(draw.hDC, &draw.rcItem, border);
            DeleteObject(border);
        }
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, Color(primary ? state->theme.accent_text : state->theme.text));
        HGDIOBJ old = state->font ? SelectObject(draw.hDC, state->font) : nullptr;
        wchar_t text[128]{};
        GetWindowTextW(draw.hwndItem, text, ARRAYSIZE(text));
        RECT text_rect = draw.rcItem;
        DrawTextW(draw.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (draw.itemState & ODS_FOCUS) {
            InflateRect(&text_rect, -3, -3);
            DrawFocusRect(draw.hDC, &text_rect);
        }
        if (old) SelectObject(draw.hDC, old);
        return TRUE;
    }
    if (msg == WM_COMMAND) {
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
    state.theme = ui::MakeTheme(dark, accent);
    state.background = CreateSolidBrush(state.high_contrast ? GetSysColor(COLOR_WINDOW) :
        Color(state.theme.surface_sheet));
    state.input = CreateSolidBrush(state.high_contrast ? GetSysColor(COLOR_WINDOW) :
        Color(state.theme.fill_input_focus));
    struct Brushes {
        DialogState& state;
        ~Brushes() { DeleteObject(state.background); DeleteObject(state.input); }
    } brushes{state};
    state.max_length = max_length;
    state.confirm_label = confirm_label;
    state.dpi = compat::WindowDpi(owner);
    const DWORD style = state.high_contrast ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU : WS_POPUP;
    const DWORD ex_style = WS_EX_CONTROLPARENT | (state.high_contrast ? WS_EX_DLGMODALFRAME : 0);
    RECT bounds{0, 0, MulDiv(360, static_cast<int>(state.dpi), 96),
        MulDiv(state.high_contrast ? 144 : 180, static_cast<int>(state.dpi), 96)};
    if (state.high_contrast) {
        using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const auto adjust = reinterpret_cast<AdjustForDpi>(compat::UserApi("AdjustWindowRectExForDpi"));
        if (!compat::LegacyMode() && adjust) adjust(&bounds, style, FALSE, ex_style, state.dpi);
        else AdjustWindowRectEx(&bounds, style, FALSE, ex_style);
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    RECT parent{};
    GetWindowRect(owner, &parent);
    const int x = parent.left + ((parent.right-parent.left)-width)/2;
    const int y = parent.top + ((parent.bottom-parent.top)-height)/2;
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
