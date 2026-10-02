#include "../ui/dialog_lifecycle.h"
#include "archive_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../common/path_utils.h"
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
constexpr int kDestination = 100, kPassword = 101, kBrowse = 102, kCreateFolder = 103;

const wchar_t* Label(const wchar_t* chinese, const wchar_t* english) {
    return l10n::effective_language() == l10n::Language::ZhCN ? chinese : english;
}

struct DialogState {
    bool dark = false, password_only = false, progress = false, all = false, accepted = false;
    COLORREF accent = RGB(80, 180, 220);
    UINT dpi = 96;
    ArchiveExtractOptions options;
    std::wstring folder_name;
    HWND destination = nullptr, password = nullptr, confirm = nullptr;
    HWND modal_owner = nullptr;
    HFONT font = nullptr, title_font = nullptr;
    HBRUSH edit_brush = nullptr;
    std::atomic_bool* done = nullptr;
    std::atomic_bool* cancel = nullptr;
    ~DialogState() {
        if (font) DeleteObject(font);
        if (title_font) DeleteObject(title_font);
        if (edit_brush) DeleteObject(edit_brush);
    }
};

int Px(const DialogState& s, int value) { return MulDiv(value, static_cast<int>(s.dpi), 96); }
int Width(const DialogState& s) { return s.password_only || s.progress ? 420 : 520; }
int FooterTop(const DialogState& s) { return s.progress ? 136 : s.password_only ? 160 : s.all ? 284 : 242; }
COLORREF Background(const DialogState& s) { return s.dark ? RGB(42, 42, 42) : RGB(250, 250, 250); }
COLORREF Footer(const DialogState& s) { return s.dark ? RGB(35, 35, 35) : RGB(242, 242, 242); }
COLORREF Text(const DialogState& s) { return s.dark ? RGB(248, 248, 248) : RGB(28, 28, 28); }
COLORREF Input(const DialogState& s) { return s.dark ? RGB(31, 31, 31) : RGB(255, 255, 255); }

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
    Fill(dc, {0, Px(s, FooterTop(s)), client.right, client.bottom}, Footer(s));
    Fill(dc, {0, Px(s, FooterTop(s)), client.right, Px(s, FooterTop(s) + 1)},
         s.dark ? RGB(55, 55, 55) : RGB(220, 220, 220));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Text(s));
    HGDIOBJ old_font = SelectObject(dc, s.title_font);
    RECT title{Px(s, 24), Px(s, 20), Px(s, Width(s) - 24), Px(s, 54)};
    DrawTextW(dc, s.password_only ? Label(L"压缩包密码", L"Archive password")
        : Label(L"解压压缩包", L"Extract archive"), -1, &title, DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, s.font);
    const auto draw_input = [&](int y, int right, const wchar_t* label) {
        RECT prompt{Px(s, 24), Px(s, y - 32), Px(s, Width(s) - 24), Px(s, y - 8)};
        DrawTextW(dc, label, -1, &prompt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        Rounded(dc, {Px(s, 24), Px(s, y), Px(s, right), Px(s, y + 34)}, Input(s),
                s.dark ? RGB(62, 62, 62) : RGB(196, 196, 196), Px(s, 5));
        Fill(dc, {Px(s, 26), Px(s, y + 32), Px(s, right - 2), Px(s, y + 34)}, s.accent);
    };
    if (s.progress) {
        RECT status{Px(s, 24), Px(s, 70), Px(s, Width(s) - 24), Px(s, 106)};
        DrawTextW(dc, s.cancel->load(std::memory_order_acquire)
            ? Label(L"正在取消…", L"Cancelling…") : Label(L"正在解压…", L"Extracting…"),
            -1, &status, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        if (!s.password_only) draw_input(100, Width(s) - 124, Label(L"解压到", L"Destination"));
        draw_input(s.password_only ? 100 : 192, Width(s) - 24,
            s.password_only ? Label(L"请输入密码", L"Enter password")
                            : Label(L"密码（如需要）", L"Password (if required)"));
    }
    HPEN border = CreatePen(PS_SOLID, 1, s.dark ? RGB(62, 75, 84) : RGB(188, 196, 202));
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, 0, 0, client.right - 1, client.bottom - 1, Px(s, 10), Px(s, 10));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);
    SelectObject(dc, old_font);
    EndPaint(hwnd, &paint);
}

void PaintButton(const DialogState& s, const DRAWITEMSTRUCT& item) {
    const bool checkbox = item.CtlID == kCreateFolder;
    const bool primary = item.CtlID == IDOK;
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    Fill(item.hDC, item.rcItem, checkbox || item.CtlID == kBrowse ? Background(s) : Footer(s));
    RECT text = item.rcItem;
    if (checkbox) {
        RECT box{item.rcItem.left, item.rcItem.top + Px(s, 5),
            item.rcItem.left + Px(s, 18), item.rcItem.top + Px(s, 23)};
        Rounded(item.hDC, box, s.options.create_folder ? s.accent : Input(s),
            s.options.create_folder ? s.accent : RGB(145, 145, 145), Px(s, 3));
        if (s.options.create_folder) {
            SetTextColor(item.hDC, RGB(0, 0, 0));
            SetBkMode(item.hDC, TRANSPARENT);
            DrawTextW(item.hDC, L"✓", -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        text.left += Px(s, 27);
    } else {
        const COLORREF fill = primary && enabled ? s.accent : s.dark
            ? (pressed ? RGB(65, 65, 65) : RGB(53, 53, 53))
            : (pressed ? RGB(220, 220, 220) : RGB(232, 232, 232));
        Rounded(item.hDC, item.rcItem, fill, fill, Px(s, 5));
    }
    const bool light_accent = 0.2126f * GetRValue(s.accent) +
        0.7152f * GetGValue(s.accent) + 0.0722f * GetBValue(s.accent) > 160.0f;
    SetTextColor(item.hDC, !enabled ? RGB(145, 145, 145) : primary
        ? (light_accent ? RGB(0, 0, 0) : RGB(255, 255, 255)) : Text(s));
    SetBkMode(item.hDC, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(item.hDC, s.font);
    wchar_t label[512]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
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
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog)))) return;
    DWORD flags = 0;
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(Label(L"选择解压目标文件夹", L"Choose extraction folder"));
    const std::wstring path = ReadEdit(s.destination);
    IShellItem* initial = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
        initial->Release();
    }
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(dialog->GetResult(&folder))) {
            PWSTR selected = nullptr;
            if (SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &selected)) && selected) {
                SetWindowTextW(s.destination, selected);
                CoTaskMemFree(selected);
            }
            folder->Release();
        }
    }
    dialog->Release();
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, wp == VK_RETURN ? IDOK : IDCANCEL, 0);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void RequestCancel(HWND hwnd, DialogState& state) {
    state.cancel->store(true, std::memory_order_release);
    EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        s = static_cast<DialogState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_CREATE) {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        LOGFONTW font{};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            font = metrics.lfMessageFont;
        else wcscpy_s(font.lfFaceName, L"Segoe UI");
        font.lfHeight = -Px(*s, 14);
        s->font = CreateFontIndirectW(&font);
        font.lfHeight = -Px(*s, 18);
        font.lfWeight = FW_SEMIBOLD;
        s->title_font = CreateFontIndirectW(&font);
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
            return child;
        };
        if (s->progress) {
            button(IDCANCEL, l10n::Get(l10n::StringId::Cancel).c_str(),
                Width(*s) - 170, FooterTop(*s) + 24, 146);
            if (!SetTimer(hwnd, 1, 100, nullptr)) return -1;
            return 0;
        }
        if (!s->password_only) {
            s->destination = edit(kDestination, 100, Width(*s) - 124, s->options.destination, false);
            button(kBrowse, Label(L"浏览…", L"Browse…"), Width(*s) - 112, 101, 88);
        }
        s->password = edit(kPassword, s->password_only ? 100 : 192, Width(*s) - 24,
                           s->options.password, true);
        if (!s->password_only && s->all) {
            std::wstring label = Label(L"创建文件夹：", L"Create folder: ");
            label += s->folder_name;
            button(kCreateFolder, label.c_str(), 24, 240, Width(*s) - 48);
        }
        const int button_width = (Width(*s) - 58) / 2;
        s->confirm = button(IDOK, s->password_only ? Label(L"确定", L"OK")
            : Label(L"解压", L"Extract"), 24, FooterTop(*s) + 24, button_width);
        button(IDCANCEL, l10n::Get(l10n::StringId::Cancel).c_str(),
            34 + button_width, FooterTop(*s) + 24, button_width);
        EnableWindow(s->confirm, s->password_only || !s->options.destination.empty());
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_TIMER && s->progress && wp == 1) {
        if (s->done->load(std::memory_order_acquire)) {
            s->accepted = !s->cancel->load(std::memory_order_acquire);
            ui::DestroyDialogWithFade(hwnd, s->modal_owner);
        } else if (s->cancel->load(std::memory_order_acquire)) {
            EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    if (msg == WM_PAINT) { Paint(hwnd, *s); return 0; }
    if (msg == WM_DRAWITEM) { PaintButton(*s, *reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; }
    if (msg == WM_CTLCOLOREDIT) {
        SetBkColor(reinterpret_cast<HDC>(wp), Input(*s));
        SetTextColor(reinterpret_cast<HDC>(wp), Text(*s));
        return reinterpret_cast<LRESULT>(s->edit_brush);
    }
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (s->progress) {
            if (id == IDCANCEL) RequestCancel(hwnd, *s);
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
        if (id == IDCANCEL) { ui::DestroyDialogWithFade(hwnd, s->modal_owner); return 0; }
    }
    if (msg == WM_CLOSE) {
        if (s->progress) RequestCancel(hwnd, *s);
        else ui::DestroyDialogWithFade(hwnd, s->modal_owner);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        if (s->progress) KillTimer(hwnd, 1);
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
    const int width = Px(s, Width(s)), height = Px(s, FooterTop(s) + 80);
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
        s.password_only ? Label(L"压缩包密码", L"Archive password") : Label(L"解压压缩包", L"Extract archive"),
        WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, wc.hInstance, &s);
    if (!dialog) return false;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(s, 10), Px(s, 10));
    if (region && !SetWindowRgn(dialog, region, TRUE)) DeleteObject(region);
    const BOOL dark = s.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ui::OwnerDimScope owner_dim(owner);
    const bool owner_enabled = IsWindowEnabled(owner) != FALSE;
    s.modal_owner = owner_enabled ? owner : nullptr;
    if (owner_enabled) EnableWindow(owner, FALSE);
    ui::ShowDialogWithFade(dialog);
    SetForegroundWindow(dialog);
    SetFocus(s.progress ? GetDlgItem(dialog, IDCANCEL) : s.password_only ? s.password : s.destination);
    MSG msg{};
    int status = 1;
    while (IsWindow(dialog) && (status = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (s.progress && status <= 0) s.cancel->store(true, std::memory_order_release);
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

bool ShowArchiveProgressDialog(HWND owner, bool dark, COLORREF accent,
                               std::atomic_bool& done, std::atomic_bool& cancel) {
    DialogState state;
    state.dark = dark;
    state.accent = accent;
    state.progress = true;
    state.done = &done;
    state.cancel = &cancel;
    const bool completed = Show(owner, state);
    if (!completed) cancel.store(true, std::memory_order_release);
    return completed;
}
} // namespace pulse::app
