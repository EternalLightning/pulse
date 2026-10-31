// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/dialog_lifecycle.h"
#include <cstdio>

namespace {
HWND owner = nullptr;
HWND dialog = nullptr;
HWND editor = nullptr;
bool enabled_before_hide = true;
bool editors_hidden_first = true;
int failures = 0;
ULONGLONG repaint_until = 0;
unsigned closing_paints = 0;
void Check(bool value, const char* message) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
}
LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT && window == dialog && repaint_until) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        ++closing_paints;
        // Bound the broken implementation too, so a regression fails instead of hanging.
        if (GetTickCount64() < repaint_until) InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (msg == WM_WINDOWPOSCHANGING && (reinterpret_cast<WINDOWPOS*>(lp)->flags & SWP_HIDEWINDOW)) {
        if (window == dialog || window == editor) enabled_before_hide &= IsWindowEnabled(owner) != FALSE;
        if (window == dialog) editors_hidden_first &= !IsWindowVisible(editor);
    }
    return DefWindowProcW(window, msg, wp, lp);
}
HWND Make(HWND parent) {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PulseCloseTest", L"", WS_POPUP,
        -30000, -30000, 10, 10, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}
}
int main() {
    MSG modal_message{};
    PeekMessageW(&modal_message, nullptr, 0, 0, PM_NOREMOVE);
    Check(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 42, 17, 23) != FALSE,
          "queue ordinary modal message");
    Check(pulse::ui::GetModalMessage(modal_message) && modal_message.message == WM_APP + 42 &&
          modal_message.wParam == 17 && modal_message.lParam == 23,
          "modal message reader preserves ordinary messages");
    PostQuitMessage(73);
    Check(!pulse::ui::GetModalMessage(modal_message), "inner modal loop observes thread shutdown");
    Check(!pulse::ui::GetModalMessage(modal_message), "outer modal loop also observes thread shutdown");
    Check(GetMessageW(&modal_message, nullptr, 0, 0) == 0 && modal_message.wParam == 73,
          "main loop receives original quit exit code after nested modal loops");
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PulseCloseTest";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    owner = Make(nullptr);
    HWND unrelated = Make(nullptr);
    const HWND foreground = GetForegroundWindow();
    for (int pass = 0; pass < 2; ++pass) {
        dialog = Make(owner);
        editor = Make(dialog);
        HWND nested = Make(editor);
        ShowWindow(owner, SW_SHOWNOACTIVATE);
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
        ShowWindow(editor, SW_SHOWNOACTIVATE);
        ShowWindow(nested, SW_SHOWNOACTIVATE);
        ShowWindow(unrelated, SW_SHOWNOACTIVATE);
        EnableWindow(owner, FALSE);
        enabled_before_hide = editors_hidden_first = true;
        pulse::ui::HideComposedDialog(dialog, owner);
        Check(enabled_before_hide && IsWindowEnabled(owner), "owner enabled before any modal surface is hidden");
        Check(editors_hidden_first, "hosted editors hidden before dialog surface");
        Check(!IsWindowVisible(dialog) && !IsWindowVisible(editor) && !IsWindowVisible(nested),
              "entire owned popup tree hidden on first and repeated close");
        Check(IsWindowVisible(unrelated) && GetForegroundWindow() == foreground,
              "unrelated window visibility and foreground preserved");
        DestroyWindow(dialog);
        dialog = editor = nullptr;
    }
    dialog = Make(owner);
    ShowWindow(dialog, SW_SHOWNOACTIVATE);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    HIGHCONTRASTW close_contrast{sizeof(close_contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(close_contrast), &close_contrast, 0);
    const ULONGLONG close_started = GetTickCount64();
    repaint_until = close_started + 1500;
    InvalidateRect(dialog, nullptr, FALSE);
    pulse::ui::HideComposedDialog(dialog, owner);
    Check(GetTickCount64() - close_started < 750 && !IsWindowVisible(dialog),
        "self-invalidating progress window closes before the repaint stream stops");
    if (animations && !(close_contrast.dwFlags & HCF_HIGHCONTRASTON))
        Check(closing_paints > 1, "close animation handles repeated paint frames without draining forever");
    Check(!GetPropW(dialog, L"Pulse.DialogFadeState") && !GetPropW(dialog, L"Pulse.DialogClosing"),
        "animated progress close clears transient state");
    repaint_until = 0;
    DestroyWindow(dialog);
    dialog = Make(owner);
    pulse::ui::ShowDialogWithFade(dialog, SW_SHOWNOACTIVATE);
    PostQuitMessage(91);
    pulse::ui::HideComposedDialog(dialog, owner);
    Check(!IsWindowVisible(dialog) && !GetPropW(dialog, L"Pulse.DialogFadeState") &&
        !GetPropW(dialog, L"Pulse.DialogClosing"),
        "thread shutdown closes and clears a pending transition");
    Check(GetMessageW(&modal_message, nullptr, 0, 0) == 0 && modal_message.wParam == 91,
        "close transition preserves queued thread shutdown for the outer loop");
    DestroyWindow(dialog);
    dialog = nullptr;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    if (!(contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        HWND overlay = nullptr;
        {
            pulse::ui::OwnerDimScope dim(owner);
            dialog = Make(owner);
            pulse::ui::ShowDialogWithFade(dialog, SW_SHOWNOACTIVATE);
            const ULONGLONG deadline = GetTickCount64() + 1500;
            while (GetPropW(dialog, L"Pulse.DialogFadeState") && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
                MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            overlay = reinterpret_cast<HWND>(GetPropW(owner, L"Pulse.OwnerDimOverlay"));
            BYTE alpha = 0; COLORREF key = 0; DWORD flags = 0;
            Check(IsWindow(overlay) && IsWindowVisible(overlay) &&
                GetLayeredWindowAttributes(overlay, &key, &alpha, &flags) && alpha == 64,
                "dim overlay fades to bounded opacity without altering the owner");
            Check(SendMessageW(overlay, WM_NCHITTEST, 0, 0) == HTTRANSPARENT &&
                (GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_NOACTIVATE),
                "dim overlay neither intercepts clicks nor activates");
            {
                pulse::ui::OwnerDimScope nested_dim(owner);
                Check(GetPropW(owner, L"Pulse.OwnerDimOverlay") == overlay,
                    "nested modal scopes share the same owner dim surface");
            }
            Check(IsWindow(overlay) && GetPropW(owner, L"Pulse.OwnerDimOverlay") == overlay,
                "nested dismissal preserves the outer dim surface");
            pulse::ui::DestroyDialogWithFade(dialog);
            dialog = nullptr;
        }
        Check(!IsWindow(overlay) && !GetPropW(owner, L"Pulse.OwnerDimOverlay") &&
            GetForegroundWindow() == foreground,
            "last modal dismissal restores brightness with no foreground change");
    }
    DestroyWindow(unrelated);
    DestroyWindow(owner);
    return failures ? 1 : 0;
}
