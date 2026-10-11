#pragma once
#include <windows.h>
struct IDCompositionDevice;
struct IDCompositionVisual;

namespace pulse::ui {
inline bool IsDialogSurfaceFading(HWND window) {
    return GetPropW(window, L"Pulse.DialogSurfaceFading") != nullptr;
}
// Composition handles are borrowed only while the window's compositor is alive.
void BindDialogComposition(HWND window, IDCompositionDevice* device, IDCompositionVisual* visual);
// Fade-in returns immediately; hiding or compositor teardown cancels it.
void ShowDialogWithFade(HWND dialog);
void ShowDialogWithFade(HWND dialog, int show_command);
void DestroyDialogWithFade(HWND dialog);
void DestroyDialogWithFade(HWND dialog, HWND modal_owner);
// Hide the complete owned-window tree before tearing down composition resources.
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
}
