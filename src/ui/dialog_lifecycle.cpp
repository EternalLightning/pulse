#include "dialog_lifecycle.h"
#include <dwmapi.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::ui {
namespace {
constexpr wchar_t kDevice[] = L"Pulse.DialogComposition.Device";
constexpr wchar_t kVisual[] = L"Pulse.DialogComposition.Visual";
constexpr wchar_t kFading[] = L"Pulse.DialogFading";
bool AnimationsEnabled() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    return enabled && !(contrast.dwFlags & HCF_HIGHCONTRASTON);
}
bool OwnedBy(HWND window, HWND owner) {
    if (IsChild(owner, window)) return true;
    for (HWND current = window; current; current = GetWindow(current, GW_OWNER))
        if (current == owner) return true;
    return false;
}

void HideWithoutActivation(HWND window) {
    const BOOL disabled = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
        SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
}

void Fade(HWND window, bool show, int show_command = SW_SHOW) {
    if (!IsWindow(window) || (!show && !IsWindowVisible(window))) return;
    if (!AnimationsEnabled()) {
        if (show) ShowWindow(window, show_command);
        return;
    }
    Microsoft::WRL::ComPtr<IDCompositionDevice> device = reinterpret_cast<IDCompositionDevice*>(GetPropW(window, kDevice));
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual = reinterpret_cast<IDCompositionVisual*>(GetPropW(window, kVisual));
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    const bool composed = device && visual;
    const LONG_PTR original_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((composed && FAILED(device->CreateEffectGroup(&effect))) ||
        (!composed && (original_style & WS_EX_NOREDIRECTIONBITMAP))) {
        if (show) ShowWindow(window, show_command);
        return;
    }
    BYTE original_alpha = 255;
    COLORREF original_key = 0;
    DWORD original_flags = LWA_ALPHA;
    if (composed) {
        effect->SetOpacity(show ? 0.0f : 1.0f);
        visual->SetEffect(effect.Get());
        device->Commit();
        // Apply the initial transparent frame before making the HWND visible.
        if (show) device->WaitForCommitCompletion();
    } else {
        GetLayeredWindowAttributes(window, &original_key, &original_alpha, &original_flags);
        SetWindowLongPtrW(window, GWL_EXSTYLE, original_style | WS_EX_LAYERED);
        SetLayeredWindowAttributes(window, original_key, show ? 0 : original_alpha,
            original_flags | LWA_ALPHA);
    }
    if (show) {
        ShowWindow(window, show_command);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    const ULONGLONG start = GetTickCount64();
    const float duration = show ? 150.0f : 100.0f;
    for (;;) {
        const float t = std::min(1.0f, static_cast<float>(GetTickCount64() - start) / duration);
        const float eased = t * t * (3.0f - 2.0f * t);
        const float opacity = show ? eased : 1.0f - eased;
        if (composed) {
            effect->SetOpacity(opacity);
            device->Commit();
        } else SetLayeredWindowAttributes(window, original_key,
            static_cast<BYTE>(std::lround(opacity * original_alpha)), original_flags | LWA_ALPHA);
        DwmFlush();
        if (t >= 1.0f) break;
        Sleep(8);
    }
    if (!show) HideWithoutActivation(window);
    if (composed) {
        visual->SetEffect(nullptr);
        device->Commit();
    }
    if (!composed) {
        if (original_style & WS_EX_LAYERED)
            SetLayeredWindowAttributes(window, original_key, original_alpha, original_flags);
        else SetWindowLongPtrW(window, GWL_EXSTYLE, original_style);
    }
}
}

void ShowDialogWithFade(HWND dialog) {
    ShowDialogWithFade(dialog, SW_SHOW);
}

void ShowDialogWithFade(HWND dialog, int show_command) {
    if (!IsWindow(dialog) || IsWindowVisible(dialog)) return;
    Fade(dialog, true, show_command);
}

void DestroyDialogWithFade(HWND dialog) {
    DestroyDialogWithFade(dialog, nullptr);
}

void DestroyDialogWithFade(HWND dialog, HWND modal_owner) {
    if (!IsWindow(dialog) || GetPropW(dialog, kFading)) return;
    SetPropW(dialog, kFading, reinterpret_cast<HANDLE>(1));
    HideComposedDialog(dialog, modal_owner);
    if (IsWindow(dialog)) DestroyWindow(dialog);
}

void HideComposedDialog(HWND dialog, HWND modal_owner) {
    if (!dialog || !IsWindow(dialog)) return;
    const bool active = OwnedBy(GetActiveWindow(), dialog);
    // Windows must have an enabled activation target before the active popup disappears.
    if (modal_owner && IsWindow(modal_owner)) {
        EnableWindow(modal_owner, TRUE);
        if (active) {
            SetActiveWindow(modal_owner);
            SetFocus(modal_owner);
        }
    }
    struct OwnedWindows { HWND dialog; std::vector<HWND> windows; } owned{dialog, {}};
    EnumThreadWindows(GetWindowThreadProcessId(dialog, nullptr), [](HWND window, LPARAM param) -> BOOL {
        auto& context = *reinterpret_cast<OwnedWindows*>(param);
        if (window != context.dialog && OwnedBy(window, context.dialog)) context.windows.push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&owned));
    // Hide any remaining owned auxiliary windows before releasing composition.
    // Hide all of them while the dialog surface still covers the owner.
    for (HWND window : owned.windows) HideWithoutActivation(window);
    Fade(dialog, false);
    HideWithoutActivation(dialog);
    DwmFlush();
}
}
