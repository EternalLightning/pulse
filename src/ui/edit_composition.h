#pragma once
#include <windows.h>
#include <memory>

struct IDCompositionDevice;
struct IDCompositionEffectGroup;
struct ID2D1Device;
struct ID2D1Bitmap1;

namespace pulse::ui {
inline BYTE EditSurfaceOpacity(HWND edit) {
    const auto value = reinterpret_cast<ULONG_PTR>(GetPropW(edit, L"Pulse.EditOpacity"));
    return value ? static_cast<BYTE>(value - 1) : 255;
}

// Persistent editor pixels above native child controls, on the host's device.
class EditComposition {
public:
    EditComposition(HWND host, IDCompositionDevice* device, ID2D1Device* drawing_device,
                    IDCompositionEffectGroup* effect);
    ~EditComposition();
    bool Available() const;
    bool Present(HWND edit, ID2D1Bitmap1* bitmap);
    void Synchronize(HWND edit);
    void Remove(HWND edit);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
