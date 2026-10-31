// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_drop_shortcuts.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>

namespace pulse::app {
namespace {
std::wstring PlainPath(const std::wstring& input) {
    std::wstring value = path::StripExtendedPathPrefix(input);
    std::replace(value.begin(), value.end(), L'/', L'\\');
    wchar_t full[32768]{};
    const DWORD count = GetFullPathNameW(value.c_str(), ARRAYSIZE(full), full, nullptr);
    return count && count < ARRAYSIZE(full) ? std::wstring(full, count) : value;
}

std::wstring NativePath(const std::wstring& plain) {
    return plain.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + plain.substr(2) : L"\\\\?\\" + plain;
}

std::wstring ShortcutName(const std::wstring& target, const std::wstring& format) {
    std::wstring trimmed = target;
    while (trimmed.size() > 3 && trimmed.back() == L'\\') trimmed.pop_back();
    const auto last = trimmed.find_last_of(L'\\');
    std::wstring name = last == std::wstring::npos ? trimmed : trimmed.substr(last + 1);
    if (name.empty() && trimmed.size() >= 2 && trimmed[1] == L':') name = trimmed.substr(0, 1);
    if (name.size() >= 4 && _wcsicmp(name.c_str() + name.size() - 4, L".lnk") == 0)
        name.resize(name.size() - 4);
    std::wstring result = format;
    const auto marker = result.find(L"%s");
    if (marker == std::wstring::npos) return name;
    result.replace(marker, 2, name);
    return result;
}

void Fail(ShortcutDropResult& result, const std::wstring& source, HRESULT error) {
    ++result.failed;
    if (SUCCEEDED(result.error)) {
        result.failed_source = source;
        result.error = error;
    }
}

HRESULT SaveShortcut(IShellLinkW* link, const std::wstring& dir, const std::wstring& base) {
    Microsoft::WRL::ComPtr<IPersistStream> persist;
    HRESULT hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = persist->Save(stream.Get(), FALSE);
    if (FAILED(hr)) return hr;
    LARGE_INTEGER beginning{};
    hr = stream->Seek(beginning, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    for (unsigned number = 1; number <= 10000; ++number) {
        const std::wstring suffix = number == 1 ? L".lnk" : L" (" + std::to_wstring(number) + L").lnk";
        size_t length = (std::min)(base.size(), size_t{255} - suffix.size());
        if (length && length < base.size() && base[length - 1] >= 0xD800 && base[length - 1] <= 0xDBFF) --length;
        const std::wstring file = NativePath(dir + L"\\" + base.substr(0, length) + suffix);
        // Reserve atomically: an existing shortcut or a concurrent drop is never overwritten.
        HANDLE reservation = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return HRESULT_FROM_WIN32(error);
        }
        // IPersistFile::Save can report S_OK with an empty file for long paths.
        // Serialize once and write through the reserved handle instead.
        std::array<BYTE, 16384> buffer{};
        for (;;) {
            ULONG read = 0;
            hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);
            if (FAILED(hr) || read == 0) break;
            DWORD written = 0;
            if (!WriteFile(reservation, buffer.data(), read, &written, nullptr)) {
                hr = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            if (written != read) {
                hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
                break;
            }
        }
        CloseHandle(reservation);
        if (FAILED(hr)) DeleteFileW(file.c_str());
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
}
} // namespace

bool IsShortcutFileSystemPath(const std::wstring& input) {
    const auto value = path::StripExtendedPathPrefix(input);
    if (value.starts_with(L"\\\\.\\") || value.starts_with(L"\\\\?\\")) return false;
    if (value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') ||
        (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':' &&
        (value[2] == L'\\' || value[2] == L'/')) return true;
    if (!value.starts_with(L"\\\\")) return false;
    const auto server_end = value.find(L'\\', 2);
    return server_end != std::wstring::npos && server_end > 2 && server_end + 1 < value.size();
}

bool CanCreateDropShortcuts(const std::vector<std::wstring>& sources, const std::wstring& destination) {
    return IsShortcutFileSystemPath(destination) && !sources.empty() &&
        std::all_of(sources.begin(), sources.end(), IsShortcutFileSystemPath);
}

ShortcutDropResult CreateDropShortcuts(const std::vector<std::wstring>& sources,
    const std::wstring& destination, const std::wstring& name_format) {
    ShortcutDropResult result;
    result.destination = destination;
    if (!CanCreateDropShortcuts(sources, destination)) {
        Fail(result, destination, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        return result;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComDone { HRESULT hr; ~ComDone() { if (SUCCEEDED(hr)) CoUninitialize(); } } done{com};
    if (FAILED(com)) {
        Fail(result, destination, com);
        return result;
    }
    const auto dir = PlainPath(destination);
    const DWORD dir_attrs = GetFileAttributesW(NativePath(dir).c_str());
    if (dir_attrs == INVALID_FILE_ATTRIBUTES || !(dir_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        const DWORD error = dir_attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        Fail(result, destination, HRESULT_FROM_WIN32(error));
        return result;
    }
    for (const auto& source : sources) {
        const auto target = PlainPath(source);
        const DWORD attrs = GetFileAttributesW(NativePath(target).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            Fail(result, source, HRESULT_FROM_WIN32(GetLastError()));
            continue;
        }
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
        if (SUCCEEDED(hr)) hr = link->SetPath(target.c_str());
        if (SUCCEEDED(hr) && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            const auto separator = target.find_last_of(L'\\');
            if (separator != std::wstring::npos) {
                const auto parent = target.substr(0, separator == 2 ? 3 : separator);
                link->SetWorkingDirectory(parent.c_str());
            }
        }
        if (SUCCEEDED(hr)) hr = SaveShortcut(link.Get(), dir, ShortcutName(target, name_format));
        if (SUCCEEDED(hr)) ++result.created;
        else Fail(result, source, hr);
    }
    return result;
}
} // namespace pulse::app
