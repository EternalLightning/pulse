#pragma once
#include "path_utils.h"
#include <windows.h>
#include <shlobj.h>
#include <optional>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace pulse::path {

inline std::wstring MappedDriveLabelFromDisplayName(const std::wstring& root, std::wstring display) {
    const auto plain = StripExtendedPathPrefix(root);
    const auto suffix = L" (" + plain.substr(0, 2) + L")";
    if (display.size() >= suffix.size() &&
        EqualInsensitive(std::wstring_view(display).substr(display.size() - suffix.size()), suffix))
        display.resize(display.size() - suffix.size());
    return display;
}

// Background callers only: Shell name lookup can contact a network provider.
// The returned label excludes the drive-letter suffix used by all Pulse views.
inline std::optional<std::wstring> ReadMappedDriveLabel(const std::wstring& root) {
    if (!IsDriveRootPath(root)) return {};
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return {};
    struct ComDone { ~ComDone() { CoUninitialize(); } } com_done;
    const auto plain = StripExtendedPathPrefix(root);
    const wchar_t canonical[] = {plain.front(), L':', L'\\', 0};
    PIDLIST_ABSOLUTE item = nullptr;
    HRESULT hr = SHParseDisplayName(canonical, nullptr, &item, 0, nullptr);
    PWSTR display = nullptr;
    if (SUCCEEDED(hr)) hr = SHGetNameFromIDList(item, SIGDN_NORMALDISPLAY, &display);
    std::optional<std::wstring> label;
    if (SUCCEEDED(hr) && display && display[0])
        label = MappedDriveLabelFromDisplayName(canonical, display);
    CoTaskMemFree(display);
    CoTaskMemFree(item);
    return label;
}

} // namespace pulse::path
