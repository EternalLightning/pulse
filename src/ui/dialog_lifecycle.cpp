#include "dialog_lifecycle.h"
#include <dwmapi.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <new>

namespace pulse::ui {
namespace {
constexpr wchar_t kDevice[] = L"Pulse.DialogComposition.Device";
constexpr wchar_t kVisual[] = L"Pulse.DialogComposition.Visual";
constexpr wchar_t kFading[] = L"Pulse.DialogFading";
constexpr wchar_t kOwnerDim[] = L"Pulse.OwnerDimOverlay";
constexpr wchar_t kDimClass[] = L"PulseOwnerDimOverlay";
struct DimState { HWND owner = nullptr; unsigned scopes = 1; HWND dialog = nullptr; };

bool PlaceDimOverlay(HWND overlay, HWND owner) {
    RECT client{};
    if (!IsWindow(owner) || !GetClientRect(owner, &client)) return false;
    POINT origin{client.left, client.top};
    if (!ClientToScreen(owner, &origin)) return false;
    SetWindowPos(overlay, nullptr, origin.x, origin.y, client.right - client.left,
        client.bottom - client.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    return true;
}

LRESULT CALLBACK DimProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<DimState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const HWND owner = static_cast<HWND>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        state = new (std::nothrow) DimState{owner, 1};
        if (!state) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) {
        RECT rect{}; GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wp), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    if (message == WM_TIMER && state) { PlaceDimOverlay(hwnd, state->owner); return 0; }
    if (message == WM_NCDESTROY && state) {
        if (IsWindow(state->owner) && GetPropW(state->owner, kOwnerDim) == hwnd)
            RemovePropW(state->owner, kOwnerDim);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
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

HWND CompanionDimOverlay(HWND window, bool show) {
    const HWND owner = GetWindow(window, GW_OWNER);
    const HWND overlay = reinterpret_cast<HWND>(GetPropW(owner, kOwnerDim));
    if (!IsWindow(overlay) || overlay == window) return nullptr;
    auto* state = reinterpret_cast<DimState*>(GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (!state || state->scopes != 1) return nullptr;
    if (show && !state->dialog && !IsWindowVisible(overlay)) {
        state->dialog = window;
        return overlay;
    }
    if (!show && state->dialog == window) {
        state->dialog = nullptr;
        return overlay;
    }
    return nullptr;
}

void SetDimOpacity(HWND overlay, float opacity) {
    if (overlay) SetLayeredWindowAttributes(overlay, 0,
        static_cast<BYTE>(std::lround(opacity * 64.0f)), LWA_ALPHA);
}

void Fade(HWND window, bool show, int show_command = SW_SHOW) {
    if (!IsWindow(window) || (!show && !IsWindowVisible(window))) return;
    const HWND dim = CompanionDimOverlay(window, show);
    const auto show_immediately = [&] {
        if (dim) {
            SetDimOpacity(dim, show ? 1.0f : 0.0f);
            if (show) ShowWindow(dim, SW_SHOWNOACTIVATE);
            else HideWithoutActivation(dim);
        }
        if (show) ShowWindow(window, show_command);
    };
    if (!AnimationsEnabled()) {
        show_immediately();
        return;
    }
    Microsoft::WRL::ComPtr<IDCompositionDevice> device = reinterpret_cast<IDCompositionDevice*>(GetPropW(window, kDevice));
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual = reinterpret_cast<IDCompositionVisual*>(GetPropW(window, kVisual));
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    const bool composed = device && visual;
    const LONG_PTR original_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((composed && FAILED(device->CreateEffectGroup(&effect))) ||
        (!composed && (original_style & WS_EX_NOREDIRECTIONBITMAP))) {
        show_immediately();
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
        if (dim) {
            SetDimOpacity(dim, 0.0f);
            ShowWindow(dim, SW_SHOWNOACTIVATE);
        }
        ShowWindow(window, show_command);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    const ULONGLONG start = GetTickCount64();
    const float duration = 90.0f;
    for (;;) {
        const float t = std::min(1.0f, static_cast<float>(GetTickCount64() - start) / duration);
        const float eased = t * t * (3.0f - 2.0f * t);
        const float opacity = show ? eased : 1.0f - eased;
        if (composed) {
            effect->SetOpacity(opacity);
            device->Commit();
        } else SetLayeredWindowAttributes(window, original_key,
            static_cast<BYTE>(std::lround(opacity * original_alpha)), original_flags | LWA_ALPHA);
        SetDimOpacity(dim, opacity);
        DwmFlush();
        if (t >= 1.0f) break;
        Sleep(8);
    }
    if (!show) {
        HideWithoutActivation(window);
        if (dim) HideWithoutActivation(dim);
    }
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

OwnerDimScope::OwnerDimScope(HWND owner) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    if (!IsWindow(owner) || !IsWindowVisible(owner) || (contrast.dwFlags & HCF_HIGHCONTRASTON)) return;
    owner_ = owner;
    overlay_ = reinterpret_cast<HWND>(GetPropW(owner, kOwnerDim));
    if (IsWindow(overlay_)) {
        if (auto* state = reinterpret_cast<DimState*>(GetWindowLongPtrW(overlay_, GWLP_USERDATA))) ++state->scopes;
        return;
    }
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DimProc;
    wc.lpszClassName = kDimClass; wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!GetClassInfoW(wc.hInstance, kDimClass, &wc)) RegisterClassW(&wc);
    overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kDimClass, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, wc.hInstance, owner);
    if (!overlay_) { owner_ = nullptr; return; }
    if (!SetPropW(owner, kOwnerDim, overlay_)) {
        DestroyWindow(overlay_); owner_ = overlay_ = nullptr; return;
    }
    PlaceDimOverlay(overlay_, owner);
    SetLayeredWindowAttributes(overlay_, 0, 0, LWA_ALPHA);
    SetTimer(overlay_, 1, 50, nullptr);
}

OwnerDimScope::~OwnerDimScope() {
    if (!IsWindow(overlay_)) return;
    auto* state = reinterpret_cast<DimState*>(GetWindowLongPtrW(overlay_, GWLP_USERDATA));
    if (!state || --state->scopes != 0) return;
    if (IsWindow(owner_) && GetPropW(owner_, kOwnerDim) == overlay_) RemovePropW(owner_, kOwnerDim);
    Fade(overlay_, false);
    DestroyWindow(overlay_);
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
