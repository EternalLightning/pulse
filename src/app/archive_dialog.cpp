// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/dialog_lifecycle.h"
#include "../ui/dialog_style.h"
#include "../ui/FluentTokens.h"
#include "archive_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../common/path_utils.h"
#include "../ipc/shell_interaction_client.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace pulse::app {
namespace {
constexpr wchar_t kClass[] = L"PulseArchiveDialog";
namespace ds = ui::dialog_style;
constexpr int kDestination = 100, kPassword = 101, kBrowse = 102, kCreateFolder = 103, kClose = 104;

struct DialogState {
    bool dark = false, password_only = false, all = false, accepted = false;
    COLORREF accent = RGB(80, 180, 220);
    UINT dpi = 96;
    ArchiveExtractOptions options;
    std::wstring folder_name;
    HWND destination = nullptr, password = nullptr, confirm = nullptr;
    HWND modal_owner = nullptr;
    HFONT font = nullptr, title_font = nullptr, icon_font = nullptr, close_font = nullptr;
    bool high_contrast = false;
    ui::Theme theme{};
    HBRUSH edit_brush = nullptr;
    ~DialogState() {
        if (icon_font) DeleteObject(icon_font);
        if (close_font) DeleteObject(close_font);
        if (font) DeleteObject(font);
        if (title_font) DeleteObject(title_font);
        if (edit_brush) DeleteObject(edit_brush);
    }
};

int Px(const DialogState& s, int value) { return MulDiv(value, static_cast<int>(s.dpi), 96); }
int Width(const DialogState& s) { return s.password_only ? 420 : 520; }
int FooterTop(const DialogState& s) { return s.password_only ? 160 : s.all ? 284 : 242; }
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
    font.lfWeight = FW_SEMIBOLD;
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
    if (s.font) DeleteObject(s.font);
    if (s.title_font) DeleteObject(s.title_font);
    if (s.icon_font) DeleteObject(s.icon_font);
    if (s.close_font) DeleteObject(s.close_font);
    s.font = text; s.title_font = title; s.icon_font = icon; s.close_font = close;
    if (GetDlgItem(hwnd, kClose)) SendMessageW(GetDlgItem(hwnd, kClose), WM_SETFONT, reinterpret_cast<WPARAM>(close), TRUE);
    HDC dc = GetDC(hwnd);
    const auto old = SelectObject(dc, text);
    const auto button_width = [&](int id) {
        wchar_t label[128]{}; GetWindowTextW(GetDlgItem(hwnd, id), label, ARRAYSIZE(label));
        SIZE size{}; GetTextExtentPoint32W(dc, label, lstrlenW(label), &size);
        return std::max(Px(s, 80), static_cast<int>(size.cx) + Px(s, 32));
    };
    const int primary = button_width(IDOK), cancel = button_width(IDCANCEL);
    const int browse = button_width(kBrowse);
    SelectObject(dc, old); ReleaseDC(hwnd, dc);
    const int right = Px(s, Width(s) - static_cast<int>(ds::kMargin));
    const int gap = Px(s, static_cast<int>(ds::kGap));
    const int height = Px(s, static_cast<int>(ds::kControlHeight));
    if (s.destination) {
        MoveWindow(s.destination, Px(s, 28), Px(s, 105), right - browse - gap - Px(s, 36), Px(s, 22), TRUE);
        MoveWindow(GetDlgItem(hwnd, kBrowse), right - browse, Px(s, 100), browse, height, TRUE);
    }
    MoveWindow(s.password, Px(s, 28), Px(s, s.password_only ? 105 : 197), right - Px(s, 36), Px(s, 22), TRUE);
    if (GetDlgItem(hwnd, kCreateFolder)) MoveWindow(GetDlgItem(hwnd, kCreateFolder), Px(s, 20), Px(s, 240),
        Px(s, Width(s) - 40), height, TRUE);
    const int y = Px(s, FooterTop(s) + 16);
    MoveWindow(GetDlgItem(hwnd, IDOK), right - primary, y, primary, height, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), right - primary - gap - cancel, y, cancel, height, TRUE);
    MoveWindow(GetDlgItem(hwnd, kClose), Px(s, Width(s) - static_cast<int>(ds::kCloseWidth)), 0,
        Px(s, static_cast<int>(ds::kCloseWidth)), Px(s, static_cast<int>(ds::kTitleHeight)), TRUE);
    RECT bounds{}; GetClientRect(hwnd, &bounds);
    HRGN region = CreateRoundRectRgn(0, 0, bounds.right + 1, bounds.bottom + 1, Px(s, 16), Px(s, 16));
    if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void Rounded(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

std::wstring ReadEdit(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void Paint(HWND hwnd, const DialogState& s) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    Fill(dc, client, Background(s));
    Fill(dc, {0, 0, client.right, Px(s, static_cast<int>(ds::kTitleHeight))}, Footer(s));
    Fill(dc, {0, Px(s, FooterTop(s)), client.right, client.bottom}, Footer(s));
    Fill(dc, {0, Px(s, FooterTop(s)), client.right, Px(s, FooterTop(s) + 1)},
         Color(s.theme.stroke_divider, Footer(s)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Text(s));
    HGDIOBJ old_font = SelectObject(dc, s.title_font);
    SelectObject(dc, s.icon_font);
    RECT icon{Px(s, 20), Px(s, 12), Px(s, 40), Px(s, 32)};
    SetTextColor(dc, Color(s.high_contrast ? s.theme.text : s.theme.accent_ink, Footer(s)));
    DrawTextW(dc, s.password_only ? L"\xE72E" : L"\xE896", -1, &icon, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SetTextColor(dc, Text(s));
    SelectObject(dc, s.title_font);
    RECT title{Px(s, 48), 0, Px(s, Width(s) - 46), Px(s, 44)};
    DrawTextW(dc, s.password_only ? l10n::Get(l10n::StringId::ArchivePassword).c_str()
        : l10n::Get(l10n::StringId::ArchiveExtractTitle).c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, s.font);
    const auto draw_input = [&](int y, int right, HWND edit, const wchar_t* label) {
        RECT prompt{Px(s, 20), Px(s, y - 36), Px(s, Width(s) - 20), Px(s, y - 8)};
        DrawTextW(dc, label, -1, &prompt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        Rounded(dc, {Px(s, 20), Px(s, y), Px(s, right), Px(s, y + 32)}, Input(s),
                Color(s.high_contrast ? s.theme.stroke_card : s.dark
                    ? ui::HexColor(0xFFFFFF, 0.08f) : ui::HexColor(0x000000, 13.0f / 255.0f), Input(s)), Px(s, 16));
        Fill(dc, {Px(s, 28), Px(s, y + 30), Px(s, right - 8), Px(s, y + 32)},
            Color(GetFocus() == edit ? s.theme.accent : s.theme.stroke_input_bottom, Input(s)));
    };
    if (!s.password_only) {
        RECT browse{}; GetWindowRect(GetDlgItem(hwnd, kBrowse), &browse);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&browse), 2);
        draw_input(100, MulDiv(browse.left, 96, static_cast<int>(s.dpi)) - static_cast<int>(ds::kGap), s.destination, l10n::Get(l10n::StringId::ArchiveDestination).c_str());
    }
    draw_input(s.password_only ? 100 : 192, Width(s) - 20, s.password,
        s.password_only ? l10n::Get(l10n::StringId::ArchiveEnterPassword).c_str()
                        : l10n::Get(l10n::StringId::ArchiveOptionalPassword).c_str());
    HPEN border = CreatePen(PS_SOLID, 1, Color(s.theme.stroke_card, Background(s)));
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, 0, 0, client.right - 1, client.bottom - 1, Px(s, 16), Px(s, 16));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);
    SelectObject(dc, old_font);
    EndPaint(hwnd, &paint);
}

void PaintButton(const DialogState& s, const DRAWITEMSTRUCT& item) {
    const bool checkbox = item.CtlID == kCreateFolder;
    const bool primary = item.CtlID == IDOK;
    const bool close = item.CtlID == kClose;
    const bool hover = GetPropW(item.hwndItem, L"Pulse.ButtonHover") != nullptr;
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    Fill(item.hDC, item.rcItem, checkbox || item.CtlID == kBrowse ? Background(s) : Footer(s));
    RECT text = item.rcItem;
    const COLORREF button_background = checkbox || item.CtlID == kBrowse ? Background(s) : Footer(s);
    const auto card = ui::BlendOver(s.theme.surface_card, RgbColor(button_background));
    const auto fill_color = ui::BlendOver(primary && enabled ? (pressed ? s.theme.accent_pressed : hover ? s.theme.accent_hover : s.theme.accent)
        : close ? (pressed || hover ? (s.high_contrast ? s.theme.fill_hover : s.theme.danger) : s.theme.surface_title)
        : pressed ? ui::BlendOver(s.theme.fill_pressed, card)
        : hover ? ui::BlendOver(s.theme.fill_hover, card) : card, RgbColor(button_background));
    if (checkbox) {
        RECT box{item.rcItem.left, item.rcItem.top + Px(s, 5),
            item.rcItem.left + Px(s, 18), item.rcItem.top + Px(s, 23)};
        Rounded(item.hDC, box, s.options.create_folder ? Color(s.theme.accent) : Input(s),
            s.options.create_folder ? Color(s.theme.accent) : Color(s.theme.stroke_input_bottom, Input(s)), Px(s, 3));
        if (s.options.create_folder) {
            SetTextColor(item.hDC, s.high_contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : Color(ui::AutoAccentText(s.theme.accent)));
            HGDIOBJ check_font = SelectObject(item.hDC, s.font);
            SetBkMode(item.hDC, TRANSPARENT);
            DrawTextW(item.hDC, L"✓", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item.hDC, check_font);
        }
        text.left += Px(s, 27);
    } else {
        Rounded(item.hDC, item.rcItem, Color(fill_color),
            Color(primary || close ? fill_color : s.theme.stroke_card, Color(fill_color)), Px(s, 16));
    }
    const bool highlighted = s.high_contrast && (primary || (!checkbox && (hover || pressed)));
    SetTextColor(item.hDC, !enabled ? Color(s.theme.text_disabled, checkbox ? button_background : Color(fill_color)) : highlighted ? GetSysColor(COLOR_HIGHLIGHTTEXT)
        : primary || (close && (hover || pressed)) ? Color(ui::AutoAccentText(fill_color)) : Color(s.theme.text, checkbox ? button_background : Color(fill_color)));
    SetBkMode(item.hDC, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(item.hDC, close ? s.close_font : s.font);
    wchar_t label[512]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    if (close) wcscpy_s(label, L"\xE8BB");
    DrawTextW(item.hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
        DT_NOPREFIX | (checkbox ? DT_LEFT : DT_CENTER));
    SelectObject(item.hDC, old_font);
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Px(s, 3), -Px(s, 3));
        DrawFocusRect(item.hDC, &focus);
    }
}

void Browse(HWND owner, DialogState& s) {
    std::wstring selected;
    if (ipc::PickShellPath(owner, true, l10n::Get(l10n::StringId::ArchivePickDestination), selected,
            {}, ReadEdit(s.destination))) SetWindowTextW(s.destination, selected.c_str());
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, wp == VK_RETURN ? IDOK : IDCANCEL, 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_CREATE) {
        s->edit_brush = CreateSolidBrush(Input(*s));
        const auto edit = [&](int id, int y, int right, const std::wstring& value, bool secret) {
            HWND child = CreateWindowExW(0, L"EDIT", value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0),
                Px(*s, 33), Px(*s, y + 6), Px(*s, right - 42), Px(*s, 24),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            SendMessageW(child, EM_SETLIMITTEXT, secret ? 4096 : 32767, 0);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            SetWindowSubclass(child, EditProc, 1, 0);
            return child;
        };
        const auto button = [&](int id, const wchar_t* label, int x, int y, int width) {
            HWND child = CreateWindowExW(0, L"BUTTON", label,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                Px(*s, x), Px(*s, y), Px(*s, width), Px(*s, 32), hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(s->font), TRUE);
            SetWindowSubclass(child, ButtonProc, 1, 0);
            return child;
        };
        if (!s->password_only) {
            s->destination = edit(kDestination, 100, Width(*s) - 124, s->options.destination, false);
            const std::wstring browse_label = l10n::Get(l10n::StringId::DupBrowse) + L"…";
            button(kBrowse, browse_label.c_str(), Width(*s) - 112, 101, 88);
        }
        s->password = edit(kPassword, s->password_only ? 100 : 192, Width(*s) - 24,
                           s->options.password, true);
        if (!s->password_only && s->all) {
            std::wstring label = l10n::Get(l10n::StringId::ArchiveCreateFolderPrefix);
            label += s->folder_name;
            button(kCreateFolder, label.c_str(), 24, 240, Width(*s) - 48);
        }
        const int button_width = (Width(*s) - 58) / 2;
        s->confirm = button(IDOK, s->password_only ? l10n::Get(l10n::StringId::ConfirmDefault).c_str()
            : l10n::Get(l10n::StringId::ArchiveExtract).c_str(), 24, FooterTop(*s) + 24, button_width);
        button(IDCANCEL, l10n::Get(l10n::StringId::Cancel).c_str(),
            34 + button_width, FooterTop(*s) + 24, button_width);
        HWND close = button(kClose, L"Close", 0, 0, 0);
        SetWindowLongPtrW(close, GWL_STYLE, GetWindowLongPtrW(close, GWL_STYLE) & ~WS_TABSTOP);
        Layout(hwnd, *s);
        EnableWindow(s->confirm, s->password_only || !s->options.destination.empty());
        return 0;
    }
    if (msg == WM_THEMECHANGED || msg == WM_SETTINGCHANGE) {
        s->high_contrast = ui::IsHighContrast();
        const auto accent = s->accent;
        s->theme = s->high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(s->dark,
            D2D1::ColorF(GetRValue(accent) / 255.0f, GetGValue(accent) / 255.0f, GetBValue(accent) / 255.0f));
        if (s->edit_brush) DeleteObject(s->edit_brush);
        s->edit_brush = CreateSolidBrush(Input(*s));
        ui::UpdateWindowTheme(hwnd, s->dark && !s->high_contrast);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return 0;
    }
    if (msg == WM_DPICHANGED) {
        s->dpi = HIWORD(wp);
        const auto* rect = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(hwnd, *s);
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd, &point);
        if (point.y >= 0 && point.y < Px(*s, static_cast<int>(ds::kTitleHeight)) &&
            point.x >= 0 && point.x < Px(*s, Width(*s) - static_cast<int>(ds::kCloseWidth))) return HTCAPTION;
        return HTCLIENT;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) { Paint(hwnd, *s); return 0; }
    if (msg == WM_DRAWITEM) { PaintButton(*s, *reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
    if (msg == WM_CTLCOLOREDIT) {
        SetBkColor(reinterpret_cast<HDC>(wp), Input(*s));
        SetTextColor(reinterpret_cast<HDC>(wp), Text(*s));
        return reinterpret_cast<LRESULT>(s->edit_brush);
    }
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if ((id == kDestination || id == kPassword) && (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS)) {
            const int y = id == kDestination || s->password_only ? 100 : 192;
            const RECT frame{Px(*s, 20), Px(*s, y), Px(*s, Width(*s) - 20), Px(*s, y + 32)};
            InvalidateRect(hwnd, &frame, FALSE);
            return 0;
        }
        if (id == kDestination && HIWORD(wp) == EN_CHANGE) {
            if (s->confirm) EnableWindow(s->confirm, GetWindowTextLengthW(s->destination) != 0);
            return 0;
        }
        if (id == kBrowse) { Browse(hwnd, *s); return 0; }
        if (id == kCreateFolder) {
            s->options.create_folder = !s->options.create_folder;
            InvalidateRect(reinterpret_cast<HWND>(lp), nullptr, FALSE);
            return 0;
        }
        if (id == IDOK) {
            if (!IsWindowEnabled(s->confirm)) return 0;
            if (s->destination) s->options.destination = ReadEdit(s->destination);
            s->options.password = ReadEdit(s->password);
            s->accepted = true;
            ui::DestroyDialogWithFade(hwnd, s->modal_owner);
            return 0;
        }
        if (id == IDCANCEL || id == kClose) { ui::DestroyDialogWithFade(hwnd, s->modal_owner); return 0; }
    }
    if (msg == WM_CLOSE) {
        ui::DestroyDialogWithFade(hwnd, s->modal_owner);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Show(HWND owner, DialogState& s) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    s.dpi = compat::WindowDpi(owner);
    s.high_contrast = ui::IsHighContrast();
    s.theme = s.high_contrast ? ui::MakeHighContrastTheme() : ui::MakeTheme(s.dark,
        D2D1::ColorF(GetRValue(s.accent) / 255.0f, GetGValue(s.accent) / 255.0f, GetBValue(s.accent) / 255.0f));
    const int width = Px(s, Width(s)), height = Px(s, FooterTop(s) + static_cast<int>(ds::kFooterHeight));
    RECT parent{};
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        monitor.rcWork = parent;
    const int x = std::clamp(parent.left + (parent.right - parent.left - width) / 2,
        monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    const int y = std::clamp(parent.top + (parent.bottom - parent.top - height) / 2,
        monitor.rcWork.top, std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    HWND dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClass,
        s.password_only ? l10n::Get(l10n::StringId::ArchivePassword).c_str()
                        : l10n::Get(l10n::StringId::ArchiveExtractTitle).c_str(),
        WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, wc.hInstance, &s);
    if (!dialog) return false;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(s, 16), Px(s, 16));
    if (region && !SetWindowRgn(dialog, region, TRUE)) DeleteObject(region);
    const BOOL dark = s.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ui::OwnerDimScope owner_dim(owner);
    const bool owner_enabled = IsWindowEnabled(owner) != FALSE;
    s.modal_owner = owner_enabled ? owner : nullptr;
    if (owner_enabled) EnableWindow(owner, FALSE);
    ui::ShowDialogWithFade(dialog);
    SetForegroundWindow(dialog);
    SetFocus(s.password_only ? s.password : s.destination);
    MSG msg{};
    int status = 1;
    while (IsWindow(dialog) && (status = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(dialog)) ui::DestroyDialogWithFade(dialog, s.modal_owner);
    if (status == 0) PostQuitMessage(static_cast<int>(msg.wParam));
    if (owner_enabled && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
    }
    return s.accepted;
}
} // namespace

bool ShowArchiveExtractDialog(HWND owner, bool dark, COLORREF accent,
                              const std::wstring& archive_path, bool all,
                              ArchiveExtractOptions& options) {
    DialogState state;
    state.dark = dark;
    state.accent = accent;
    state.all = all;
    state.options = options;
    const std::filesystem::path path(archive_path);
    if (state.options.destination.empty())
        state.options.destination = pulse::path::StripExtendedPathPrefix(path.parent_path().wstring());
    state.folder_name = path.stem().wstring();
    if (!all) state.options.create_folder = false;
    if (!Show(owner, state)) return false;
    options = std::move(state.options);
    return true;
}

bool ShowArchivePasswordDialog(HWND owner, bool dark, COLORREF accent, std::wstring& password) {
    DialogState state;
    state.dark = dark;
    state.accent = accent;
    state.password_only = true;
    state.options.password = password;
    if (!Show(owner, state)) return false;
    password = std::move(state.options.password);
    return true;
}
} // namespace pulse::app
