// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <sherrors.h>
#include <cwchar>
#include <string>

namespace pulse::format {

inline DWORD FileOperationMessageCode(HRESULT error) noexcept {
    switch (error) {
    case COPYENGINE_E_SHARING_VIOLATION_SRC:
    case COPYENGINE_E_SHARING_VIOLATION_DEST: return ERROR_SHARING_VIOLATION;
    case COPYENGINE_E_ACCESS_DENIED_SRC:
    case COPYENGINE_E_ACCESS_DENIED_DEST: return ERROR_ACCESS_DENIED;
    case COPYENGINE_E_PATH_NOT_FOUND_SRC:
    case COPYENGINE_E_PATH_NOT_FOUND_DEST: return ERROR_PATH_NOT_FOUND;
    case COPYENGINE_E_NET_DISCONNECT_SRC:
    case COPYENGINE_E_NET_DISCONNECT_DEST: return ERROR_CONNECTION_UNAVAIL;
    case COPYENGINE_E_ALREADY_EXISTS_NORMAL:
    case COPYENGINE_E_ALREADY_EXISTS_READONLY:
    case COPYENGINE_E_ALREADY_EXISTS_SYSTEM:
    case COPYENGINE_E_ALREADY_EXISTS_FOLDER: return ERROR_ALREADY_EXISTS;
    case COPYENGINE_E_DISK_FULL:
    case COPYENGINE_E_DISK_FULL_CLEAN:
    case COPYENGINE_E_REMOVABLE_FULL: return ERROR_DISK_FULL;
    case COPYENGINE_E_PATH_TOO_DEEP_SRC:
    case COPYENGINE_E_PATH_TOO_DEEP_DEST:
    case COPYENGINE_E_RECYCLE_PATH_TOO_LONG:
    case COPYENGINE_E_NEWFILE_NAME_TOO_LONG:
    case COPYENGINE_E_NEWFOLDER_NAME_TOO_LONG: return ERROR_FILENAME_EXCED_RANGE;
    case COPYENGINE_E_FILE_TOO_LARGE:
    case COPYENGINE_E_RECYCLE_SIZE_TOO_BIG: return ERROR_FILE_TOO_LARGE;
    case COPYENGINE_E_DIR_NOT_EMPTY: return ERROR_DIR_NOT_EMPTY;
    case COPYENGINE_E_ACCESSDENIED_READONLY: return ERROR_WRITE_PROTECT;
    case COPYENGINE_E_REQUIRES_ELEVATION: return ERROR_ELEVATION_REQUIRED;
    default:
        // A Shell HRESULT's low word is not a Win32 error code.
        return HRESULT_FACILITY(error) == FACILITY_WIN32 ? HRESULT_CODE(error) : static_cast<DWORD>(error);
    }
}

inline std::wstring FileOperationErrorText(HRESULT error, LANGID language = 0) {
    if (SUCCEEDED(error)) return {};
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD code = FileOperationMessageCode(error);
    DWORD size = FormatMessageW(flags, nullptr, code, language, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    if (!size && language != 0)
        size = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring text = size && raw ? std::wstring(raw, size) : std::wstring{};
    if (raw) LocalFree(raw);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    wchar_t number[16]{};
    swprintf_s(number, L"0x%08X", static_cast<unsigned>(error));
    if (!text.empty()) text += L" (" + std::wstring(number) + L")";
    else text = number;
    return text;
}

} // namespace pulse::format
