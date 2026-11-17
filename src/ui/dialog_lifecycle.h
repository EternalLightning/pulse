// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
struct IDCompositionDevice;
struct IDCompositionVisual;
struct IDCompositionEffectGroup;

namespace pulse::ui {
// A nested modal loop must leave thread shutdown for the outer message loop.
inline bool GetModalMessage(MSG& message) {
    const BOOL result = GetMessageW(&message, nullptr, 0, 0);
    if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
    return result > 0;
}
inline bool IsDialogSurfaceFading(HWND window) {
    return GetPropW(window, L"Pulse.DialogSurfaceFading") != nullptr;
}
// Composition handles are borrowed only while the window's compositor is alive.
void BindDialogComposition(HWND window, IDCompositionDevice* device, IDCompositionVisual* visual,
                           IDCompositionEffectGroup* effect = nullptr);
inline BYTE DialogChildOpacity(HWND child, BYTE opacity) {
    if (GetPropW(child, L"Pulse.DialogNativeEditSnapshot")) return 0;
    const auto fade = reinterpret_cast<ULONG_PTR>(GetPropW(GetAncestor(child, GA_ROOT), L"Pulse.DialogChildOpacity"));
    return fade ? static_cast<BYTE>((opacity * (fade - 1) + 127) / 255) : opacity;
}
inline UINT DialogEditFadeMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.DialogEditFade");
    return message;
}
// Fade-in returns immediately; closing reverses it and compositor teardown cancels it.
void ShowDialogWithFade(HWND dialog);
void ShowDialogWithFade(HWND dialog, int show_command);
void DestroyDialogWithFade(HWND dialog);
void DestroyDialogWithFade(HWND dialog, HWND modal_owner);
// Fade out, then hide the complete owned-window tree before returning. Composition
// resources must remain alive until this returns; closing processes only paint messages.
// Pass the disabled owner for a modal dialog; omit it for a modeless window.
void HideComposedDialog(HWND dialog, HWND modal_owner = nullptr);

// Dim only an owned window's client area while a modal surface is present.
// Nested scopes share one overlay; it never activates or steals keyboard focus.
// The overlay follows its dialog's fade clock rather than animating in this scope.
class OwnerDimScope {
public:
    explicit OwnerDimScope(HWND owner);
    ~OwnerDimScope();
    OwnerDimScope(const OwnerDimScope&) = delete;
    OwnerDimScope& operator=(const OwnerDimScope&) = delete;
private:
    HWND owner_ = nullptr;
    HWND overlay_ = nullptr;
};

class ModalOwnerScope {
public:
    explicit ModalOwnerScope(HWND owner)
        : owner_(owner), restore_(owner && IsWindowEnabled(owner)), dim_(owner) {
        if (restore_) EnableWindow(owner_, FALSE);
    }
    ~ModalOwnerScope() {
        if (restore_ && IsWindow(owner_)) EnableWindow(owner_, TRUE);
    }
    ModalOwnerScope(const ModalOwnerScope&) = delete;
    ModalOwnerScope& operator=(const ModalOwnerScope&) = delete;
    HWND DisabledOwner() const { return restore_ ? owner_ : nullptr; }
private:
    HWND owner_ = nullptr;
    bool restore_ = false;
    OwnerDimScope dim_;
};
}
