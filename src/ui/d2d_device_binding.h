#pragma once
#include "ui_compositor.h"
#include <utility>

namespace pulse::ui {
class D2DDeviceBinding {
public:
    bool Bind(ID2D1DeviceContext* window_dc) {
        ComPtr<ID2D1Device> device;
        if (window_dc) window_dc->GetDevice(&device);
        if (device.get() == device_.get() && (dc_.get() || !device.get())) return false;
        dc_.reset();
        if (device.get()) device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
        device_ = std::move(device);
        return true;
    }
    ID2D1DeviceContext* Dc() const { return dc_.get(); }
private:
    ComPtr<ID2D1Device> device_;
    ComPtr<ID2D1DeviceContext> dc_;
};
}
