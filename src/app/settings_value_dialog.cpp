#include "../ui/dialog_lifecycle.h"
#include "settings_value_dialog.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
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
};

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
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            state->font = CreateFontIndirectW(&metrics.lfMessageFont);
        HWND hint = CreateWindowExW(0, L"STATIC", state->hint.c_str(), WS_CHILD | WS_VISIBLE,
                        px(18), px(16), px(324), px(34), hwnd, nullptr, nullptr, nullptr);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            px(18), px(54), px(324), px(27), hwnd, nullptr, nullptr, nullptr);
        SendMessageW(state->edit, EM_SETLIMITTEXT, state->max_length, 0);
        const std::wstring apply_text = state->confirm_label.empty()
            ? l10n::Get(l10n::StringId::Apply) : state->confirm_label;
        HWND apply = CreateWindowExW(0, L"BUTTON", apply_text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        px(180), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", l10n::Get(l10n::StringId::Cancel).c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        px(264), px(97), px(78), px(29), hwnd, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        if (state->font) for (HWND child : {hint, state->edit, apply, cancel})
            if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        return 0;
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
        if (LOWORD(wp) == IDCANCEL) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    }
    if (msg == WM_CLOSE) { ui::DestroyDialogWithFade(hwnd, state->modal_owner); return 0; }
    if (msg == WM_NCDESTROY) { if (state->font) DeleteObject(state->font); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool PromptSettingsValue(HWND owner, const std::wstring& title,
                         const std::wstring& hint, const std::wstring& initial,
                         std::wstring& result, size_t max_length,
                         const std::wstring& confirm_label) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    DialogState state{hint, initial};
    state.max_length = max_length;
    state.confirm_label = confirm_label;
    state.dpi = compat::WindowDpi(owner);
    const int width = MulDiv(366, static_cast<int>(state.dpi), 96);
    const int height = MulDiv(166, static_cast<int>(state.dpi), 96);
    RECT parent{};
    GetWindowRect(owner, &parent);
    const int x = parent.left + ((parent.right-parent.left)-width)/2;
    const int y = parent.top + ((parent.bottom-parent.top)-height)/2;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, wc.hInstance, &state);
    if (!dialog) return false;
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
