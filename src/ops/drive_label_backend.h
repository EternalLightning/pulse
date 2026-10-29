#pragma once
#include "../common/drive_display_name.h"

namespace pulse::ops {

inline HRESULT SetMappedDriveAlias(const std::wstring& root, const std::wstring& label) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return com;
    struct ComDone { ~ComDone() { CoUninitialize(); } } com_done;
    PIDLIST_ABSOLUTE item = nullptr;
    HRESULT hr = SHParseDisplayName(root.c_str(), nullptr, &item, 0, nullptr);
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(hr)) hr = SHBindToParent(item, IID_PPV_ARGS(&parent), &child);
    if (SUCCEEDED(hr)) {
        PITEMID_CHILD renamed = nullptr;
        hr = parent->SetNameOf(nullptr, child, label.c_str(), SHGDN_NORMAL, &renamed);
        CoTaskMemFree(renamed);
        parent->Release();
    }
    CoTaskMemFree(item);
    return hr;
}

struct DriveLabelBackend {
    decltype(&GetDriveTypeW) drive_type = &GetDriveTypeW;
    decltype(&SetVolumeLabelW) set_volume_label = &SetVolumeLabelW;
    HRESULT (*set_mapped_alias)(const std::wstring&, const std::wstring&) = &SetMappedDriveAlias;
    std::optional<std::wstring> (*read_mapped_label)(const std::wstring&) = &path::ReadMappedDriveLabel;
};

} // namespace pulse::ops
