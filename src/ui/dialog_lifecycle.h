#pragma once
#include <windows.h>
struct IDCompositionDevice;
struct IDCompositionVisual;

namespace pulse::ui {
// Composition handles are borrowed only while the window's compositor is alive.
inline void BindDialogComposition(HWND window, IDCompositionDevice* device, IDCompositionVisual* visual) {
    if (!window) return;
    if (device && visual) {
        SetPropW(window, L"Pulse.DialogComposition.Device", device);
        SetPropW(window, L"Pulse.DialogComposition.Visual", visual);
    } else {
        RemovePropW(window, L"Pulse.DialogComposition.Device");
        RemovePropW(window, L"Pulse.DialogComposition.Visual");
    }
}
void ShowDialogWithFade(HWND dialog);
void ShowDialogWithFade(HWND dialog, int show_command);
void DestroyDialogWithFade(HWND dialog);
void DestroyDialogWithFade(HWND dialog, HWND modal_owner);
// Hide the complete owned-window tree before tearing down composition resources.
// Pass the disabled owner for a modal dialog; omit it for a modeless window.
void HideComposedDialog(HWND dialog, HWND modal_owner = nullptr);
}
