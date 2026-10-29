#include "dialog_lifecycle.h"
#include "animation_clock.h"
#include <dwmapi.h>
#include <commctrl.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <new>
#include <memory>

namespace pulse::ui {
namespace {
constexpr wchar_t kDevice[] = L"Pulse.DialogComposition.Device";
constexpr wchar_t kVisual[] = L"Pulse.DialogComposition.Visual";
constexpr wchar_t kFading[] = L"Pulse.DialogFading";
constexpr wchar_t kFadeState[] = L"Pulse.DialogFadeState";
constexpr wchar_t kClosing[] = L"Pulse.DialogClosing";
constexpr UINT_PTR kFadeSubclass = 0x50464144;
constexpr float kFadeSeconds = 0.110f;
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

class SurfaceFadeScope {
public:
    SurfaceFadeScope(HWND window, bool composed) : window_(window) {
        if (!composed) return;
        backdrop_ = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(GetPropW(window, L"Pulse.DialogBackdrop")));
        if (!backdrop_) return;
        const DWORD none = DWMSBT_NONE;
        if (!SetPropW(window, L"Pulse.DialogSurfaceFading", reinterpret_cast<HANDLE>(1))) return;
        if (FAILED(DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none)))) {
            RemovePropW(window, L"Pulse.DialogSurfaceFading");
            return;
        }
        active_ = true;
        const MARGINS margins{};
        DwmExtendFrameIntoClientArea(window, &margins);
    }
    ~SurfaceFadeScope() {
        if (!active_ || !IsWindow(window_)) return;
        RemovePropW(window_, L"Pulse.DialogSurfaceFading");
        DwmSetWindowAttribute(window_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop_, sizeof(backdrop_));
        const MARGINS margins{-1};
        DwmExtendFrameIntoClientArea(window_, &margins);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    SurfaceFadeScope(const SurfaceFadeScope&) = delete;
    SurfaceFadeScope& operator=(const SurfaceFadeScope&) = delete;
private:
    HWND window_ = nullptr;
    DWORD backdrop_ = 0;
    bool active_ = false;
};

struct FadeState {
    HWND window;
    HWND dim;
    uint64_t start = AnimationTimeMs();
    UINT_PTR timer = 0;
    LONG_PTR original_style;
    BYTE original_alpha = 255;
    COLORREF original_key = 0;
    DWORD original_flags = LWA_ALPHA;
    Microsoft::WRL::ComPtr<IDCompositionDevice> device;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    SurfaceFadeScope surface;

    FadeState(HWND hwnd, HWND overlay) : window(hwnd), dim(overlay),
        original_style(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
        device(reinterpret_cast<IDCompositionDevice*>(GetPropW(hwnd, kDevice))),
        visual(reinterpret_cast<IDCompositionVisual*>(GetPropW(hwnd, kVisual))),
        surface(hwnd, device && visual) {
        if (original_style & WS_EX_LAYERED)
            GetLayeredWindowAttributes(hwnd, &original_key, &original_alpha, &original_flags);
    }
    ~FadeState() {
        if (effect) {
            visual->SetEffect(nullptr);
            device->Commit();
        } else if (IsWindow(window)) {
            if (original_style & WS_EX_LAYERED)
                SetLayeredWindowAttributes(window, original_key, original_alpha, original_flags);
            else {
                const LONG_PTR current_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
                SetWindowLongPtrW(window, GWL_EXSTYLE, current_style & ~WS_EX_LAYERED);
            }
        }
    }
    bool Prepare(float from, float to, float seconds) {
        if (device && visual) {
            Microsoft::WRL::ComPtr<IDCompositionAnimation> animation;
            return (effect || SUCCEEDED(device->CreateEffectGroup(&effect))) &&
                SUCCEEDED(device->CreateAnimation(&animation)) &&
                SUCCEEDED(animation->AddCubic(0.0, from, 0.0f,
                    3.0f * (to - from) / (seconds * seconds),
                    -2.0f * (to - from) / (seconds * seconds * seconds))) &&
                SUCCEEDED(animation->End(seconds, to)) &&
                SUCCEEDED(effect->SetOpacity(animation.Get())) &&
                SUCCEEDED(visual->SetEffect(effect.Get())) && SUCCEEDED(device->Commit());
        }
        if (original_style & WS_EX_NOREDIRECTIONBITMAP) return false;
        const LONG_PTR current_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        SetWindowLongPtrW(window, GWL_EXSTYLE, current_style | WS_EX_LAYERED);
        return SetOpacity(from);
    }
    bool SetOpacity(float opacity) {
        SetDimOpacity(dim, opacity);
        return effect || SetLayeredWindowAttributes(window, original_key,
            static_cast<BYTE>(std::lround(opacity * original_alpha)), original_flags | LWA_ALPHA);
    }
};

LRESULT CALLBACK FadeProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR subclass, DWORD_PTR data);

FadeState* DetachFade(HWND window) {
    auto* state = static_cast<FadeState*>(GetPropW(window, kFadeState));
    if (!state) return nullptr;
    RemovePropW(window, kFadeState);
    RemoveWindowSubclass(window, FadeProc, kFadeSubclass);
    if (state->timer) KillTimer(window, state->timer);
    state->timer = 0;
    return state;
}

void CancelFade(HWND window) {
    delete DetachFade(window);
}

LRESULT CALLBACK FadeProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
    UINT_PTR subclass, DWORD_PTR data) {
    auto* state = reinterpret_cast<FadeState*>(data);
    if (message == WM_NCDESTROY) CancelFade(window);
    else if (message == WM_TIMER && wp == state->timer) {
        const float t = std::min(1.0f,
            static_cast<float>(AnimationTimeMs() - state->start) / (kFadeSeconds * 1000.0f));
        const float opacity = t * t * (3.0f - 2.0f * t);
        state->SetOpacity(opacity);
        if (t >= 1.0f) CancelFade(window);
        return 0;
    }
    (void)subclass;
    return DefSubclassProc(window, message, wp, lp);
}

void ShowFade(HWND window, int show_command) {
    const BOOL disable_transitions = TRUE;
    DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disable_transitions, sizeof(disable_transitions));
    const HWND dim = CompanionDimOverlay(window, true);
    const auto show_immediately = [&] {
        SetDimOpacity(dim, 1.0f);
        if (dim) ShowWindow(dim, SW_SHOWNOACTIVATE);
        ShowWindow(window, show_command);
    };
    if (!AnimationsEnabled()) { show_immediately(); return; }
    auto* state = new (std::nothrow) FadeState(window, dim);
    if (!state) { show_immediately(); return; }
    const bool ready = state->Prepare(0.0f, 1.0f, kFadeSeconds);
    if (!ready || !SetPropW(window, kFadeState, state)) {
        delete state;
        show_immediately();
        return;
    }
    if (!SetWindowSubclass(window, FadeProc, kFadeSubclass, reinterpret_cast<DWORD_PTR>(state)) ||
        !(state->timer = SetTimer(window, reinterpret_cast<UINT_PTR>(state), 16, nullptr))) {
        CancelFade(window);
        show_immediately();
        return;
    }
    state->start = AnimationTimeMs();
    SetDimOpacity(dim, 0.0f);
    if (dim) ShowWindow(dim, SW_SHOWNOACTIVATE);
    ShowWindow(window, show_command);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

std::unique_ptr<FadeState> FadeBeforeHide(HWND window, HWND dim) {
    std::unique_ptr<FadeState> state(DetachFade(window));
    float from = 1.0f;
    if (state) {
        const float t = std::min(1.0f,
            static_cast<float>(AnimationTimeMs() - state->start) / (kFadeSeconds * 1000.0f));
        from = t * t * (3.0f - 2.0f * t);
        if (!state->effect && state->original_alpha) {
            BYTE alpha = 0;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags))
                from = static_cast<float>(alpha) / state->original_alpha;
        }
    }
    if (!IsWindowVisible(window) || !AnimationsEnabled() || from <= 0.0f) return state;
    if (!state) state.reset(new (std::nothrow) FadeState(window, dim));
    if (!state) return state;
    state->dim = dim;
    const float seconds = kFadeSeconds * from;
    if (!state->Prepare(from, 0.0f, seconds)) return state;
    const uint64_t start = AnimationTimeMs();
    // Callers release their compositor immediately after hiding. Keep its references
    // and the final transparent frame alive until the entire owned tree is hidden.
    // Queued commands stay in the outer modal loop; only paint messages dispatch here.
    while (IsWindowVisible(window)) {
        const float t = std::min(1.0f,
            static_cast<float>(AnimationTimeMs() - start) / (seconds * 1000.0f));
        state->SetOpacity(from * (1.0f - t * t * (3.0f - 2.0f * t)));
        if (t >= 1.0f) break;
        MSG message{};
        if (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE)) break;
        // Animated dialogs may invalidate themselves from WM_PAINT. Draining
        // that queue can run forever after the visual has faded to zero.
        if (PeekMessageW(&message, window, WM_PAINT, WM_PAINT, PM_REMOVE))
            DispatchMessageW(&message);
        const auto elapsed = AnimationTimeMs() - start;
        const auto remaining = static_cast<DWORD>(std::max(0.0f, seconds * 1000.0f - static_cast<float>(elapsed)));
        if (remaining) MsgWaitForMultipleObjectsEx(0, nullptr, std::min<DWORD>(16, remaining),
            QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
    }
    return state;
}

}

void BindDialogComposition(HWND window, IDCompositionDevice* device, IDCompositionVisual* visual) {
    if (!window) return;
    CancelFade(window);
    if (device && visual) {
        SetPropW(window, kDevice, device);
        SetPropW(window, kVisual, visual);
    } else {
        RemovePropW(window, kDevice);
        RemovePropW(window, kVisual);
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
    DestroyWindow(overlay_);
}

void ShowDialogWithFade(HWND dialog) {
    ShowDialogWithFade(dialog, SW_SHOW);
}

void ShowDialogWithFade(HWND dialog, int show_command) {
    if (!IsWindow(dialog) || IsWindowVisible(dialog) || GetPropW(dialog, kClosing)) return;
    ShowFade(dialog, show_command);
}

void DestroyDialogWithFade(HWND dialog) {
    DestroyDialogWithFade(dialog, nullptr);
}

void DestroyDialogWithFade(HWND dialog, HWND modal_owner) {
    if (!IsWindow(dialog) || GetPropW(dialog, kFading) || GetPropW(dialog, kClosing)) return;
    SetPropW(dialog, kFading, reinterpret_cast<HANDLE>(1));
    HideComposedDialog(dialog, modal_owner);
    if (IsWindow(dialog)) DestroyWindow(dialog);
}

void HideComposedDialog(HWND dialog, HWND modal_owner) {
    if (!dialog || !IsWindow(dialog) || GetPropW(dialog, kClosing)) return;
    const bool marked_closing = SetPropW(dialog, kClosing, reinterpret_cast<HANDLE>(1)) != FALSE;
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
    const HWND dim = CompanionDimOverlay(dialog, false);
    auto fade = marked_closing ? FadeBeforeHide(dialog, dim) : nullptr;
    HideWithoutActivation(dialog);
    if (dim) HideWithoutActivation(dim);
    CancelFade(dialog);
    fade.reset();
    if (marked_closing) RemovePropW(dialog, kClosing);
}
}
