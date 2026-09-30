#include "../ui/dialog_lifecycle.h"
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
constexpr int kWidth = 350, kHeight = 240;

struct DialogState {
    std::wstring title, value;
    HWND edit = nullptr, create = nullptr;
    HWND modal_owner = nullptr;
    HFONT title_font = nullptr, text_font = nullptr;
    HBRUSH edit_brush = nullptr;
    bool dark = false, accepted = false, invalid = false;
    COLORREF accent = RGB(80, 180, 220);
    UINT dpi = 96;
    size_t max_length = 255;
};

int Px(const DialogState& state, int dip) {
    return MulDiv(dip, static_cast<int>(state.dpi), 96);
}

COLORREF Background(const DialogState& state) {
    return state.dark ? RGB(42, 42, 42) : RGB(250, 250, 250);
}

COLORREF Footer(const DialogState& state) {
    return state.dark ? RGB(35, 35, 35) : RGB(242, 242, 242);
}

COLORREF Text(const DialogState& state) {
    return state.dark ? RGB(248, 248, 248) : RGB(28, 28, 28);
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
    Fill(dc, {0, Px(state, 160), client.right, client.bottom}, Footer(state));
    Fill(dc, {0, Px(state, 160), client.right, Px(state, 161)},
         state.dark ? RGB(55, 55, 55) : RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Text(state));
    HGDIOBJ old_font = SelectObject(dc, state.title_font);
    RECT title{Px(state, 24), Px(state, 25), Px(state, 326), Px(state, 58)};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, state.text_font);
    const auto& prompt = l10n::Get(l10n::StringId::CreateItemNamePrompt);
    RECT hint{Px(state, 24), Px(state, 67), Px(state, 326), Px(state, 87)};
    DrawTextW(dc, prompt.c_str(), -1, &hint, DT_SINGLELINE | DT_VCENTER);
    const COLORREF border = state.dark ? RGB(62, 62, 62) : RGB(196, 196, 196);
    HBRUSH input_brush = CreateSolidBrush(state.dark ? RGB(31, 31, 31) : RGB(255, 255, 255));
    HPEN input_pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, input_brush);
    HGDIOBJ old_pen = SelectObject(dc, input_pen);
    RoundRect(dc, Px(state, 24), Px(state, 102), Px(state, 326), Px(state, 136),
              Px(state, 5), Px(state, 5));
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(input_pen);
    DeleteObject(input_brush);
    Fill(dc, {Px(state, 26), Px(state, 134), Px(state, 324), Px(state, 136)}, state.accent);
    if (state.invalid) {
        SetTextColor(dc, RGB(225, 90, 90));
        const auto& error = l10n::Get(l10n::StringId::InvalidName);
        RECT error_rect{Px(state, 25), Px(state, 138), Px(state, 325), Px(state, 156)};
        DrawTextW(dc, error.c_str(), -1, &error_rect, DT_SINGLELINE | DT_VCENTER);
    }
    HPEN border_pen = CreatePen(PS_SOLID, 1,
        state.dark ? RGB(62, 75, 84) : RGB(188, 196, 202));
    old_pen = SelectObject(dc, border_pen);
    old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, 0, 0, client.right - 1, client.bottom - 1,
              Px(state, 10), Px(state, 10));
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
    const COLORREF fill = create && enabled ? state.accent
        : state.dark ? (pressed ? RGB(65, 65, 65) : RGB(53, 53, 53))
                     : (pressed ? RGB(220, 220, 220) : RGB(232, 232, 232));
    Fill(item.hDC, item.rcItem, Footer(state));
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top,
              item.rcItem.right, item.rcItem.bottom, Px(state, 5), Px(state, 5));
    SelectObject(item.hDC, old_pen);
    SelectObject(item.hDC, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
    SetBkMode(item.hDC, TRANSPARENT);
    const bool light_accent = 0.2126f * GetRValue(state.accent) +
        0.7152f * GetGValue(state.accent) + 0.0722f * GetBValue(state.accent) > 160.0f;
    SetTextColor(item.hDC, !enabled ? (state.dark ? RGB(155, 155, 155) : RGB(145, 145, 145))
        : create ? (light_accent ? RGB(0, 0, 0) : RGB(255, 255, 255)) : Text(state));
    HGDIOBJ old_font = SelectObject(item.hDC, state.text_font);
    const auto& label = l10n::Get(create ? l10n::StringId::CreateItemAction
                                          : l10n::StringId::Cancel);
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
                          UINT_PTR, DWORD_PTR) {
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
            SetTextColor(dc, RGB(145, 145, 145));
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
        LOGFONTW font{};
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            font = metrics.lfMessageFont;
        else wcscpy_s(font.lfFaceName, L"Segoe UI");
        font.lfHeight = -Px(*state, 14);
        state->text_font = CreateFontIndirectW(&font);
        font.lfHeight = -Px(*state, 18);
        font.lfWeight = FW_SEMIBOLD;
        state->title_font = CreateFontIndirectW(&font);
        state->edit_brush = CreateSolidBrush(state->dark ? RGB(31, 31, 31) : RGB(255, 255, 255));
        state->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            Px(*state, 33), Px(*state, 108), Px(*state, 284), Px(*state, 24),
            hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, state->max_length, 0);
        SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Px(*state, 2), Px(*state, 2)));
        SendMessageW(state->edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->text_font), TRUE);
        SetWindowSubclass(state->edit, EditProc, 1, 0);
        state->create = CreateWindowExW(0, L"BUTTON",
            l10n::Get(l10n::StringId::CreateItemAction).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            Px(*state, 24), Px(*state, 184), Px(*state, 146), Px(*state, 32),
            hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", l10n::Get(l10n::StringId::Cancel).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            Px(*state, 179), Px(*state, 184), Px(*state, 146), Px(*state, 32),
            hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        EnableWindow(state->create, FALSE);
        SetFocus(state->edit);
        return 0;
    }
    if (msg == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(lp) == state->edit) {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, state->dark ? RGB(31, 31, 31) : RGB(255, 255, 255));
        SetTextColor(dc, Text(*state));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    if (msg == WM_DRAWITEM) {
        PaintButton(*state, *reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    }
    if (msg == WM_PAINT) { PaintDialog(hwnd, *state); return 0; }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, name.data(), length + 1);
            name.resize(static_cast<size_t>(length));
            const bool valid = IsValidFileName(name);
            state->invalid = !name.empty() && !valid;
            EnableWindow(state->create, valid);
            InvalidateRect(hwnd, nullptr, FALSE);
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
        if (LOWORD(wp) == IDCANCEL) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    }
    if (msg == WM_CLOSE) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    if (msg == WM_NCDESTROY) {
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
        kClass, title.c_str(), WS_POPUP, x, y, width, height,
        owner, nullptr, wc.hInstance, &state);
    if (!dialog) return false;
    HRGN round = CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(state, 10), Px(state, 10));
    if (round && !SetWindowRgn(dialog, round, TRUE)) DeleteObject(round);
    const BOOL use_dark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark, sizeof(use_dark));
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
