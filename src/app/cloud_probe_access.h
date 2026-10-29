#pragma once
#include <windows.h>
#include <propsys.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdint>

namespace pulse::app::cloud_probe_access {

// System.StorageProviderState.
inline constexpr PROPERTYKEY kStorageProviderState = {
    {0xE77E90DF, 0x6271, 0x4F5B, {0x83, 0x4F, 0x2D, 0xD1, 0xF2, 0x45, 0xDD, 0xA4}}, 3};

template<class ShellItem>
uint32_t ReadState(ShellItem& item) {
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    // Default getters can load the file's property handler. Missing fast
    // metadata must stay unknown rather than falling back to a content read.
    if (FAILED(item.GetPropertyStoreForKeys(&kStorageProviderState, 1,
            GPS_FASTPROPERTIESONLY, IID_PPV_ARGS(&store))) || !store)
        return 0;
    PROPVARIANT value{};
    const HRESULT result = store->GetValue(kStorageProviderState, &value);
    const uint32_t state = SUCCEEDED(result) && value.vt == VT_UI4 ? value.ulVal : 0;
    PropVariantClear(&value);
    return state;
}

} // namespace pulse::app::cloud_probe_access
