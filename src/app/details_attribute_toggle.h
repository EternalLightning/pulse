// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pulse::app {
struct Pane;

struct DetailsAttributeResult {
    Pane* pane = nullptr;
    std::weak_ptr<void> pane_lifetime;
    uint64_t tab_identity = 0;
    uint64_t view_generation = 0;
    std::wstring path;
    DWORD error = ERROR_SUCCESS;
};

// Called on the I/O queue. A handle keeps the read and write on one object;
// serializing toggles also preserves rapid clicks on either attribute.
inline DWORD ToggleDetailsAttribute(const std::wstring& path, DWORD flag) {
    if (flag != FILE_ATTRIBUTE_READONLY && flag != FILE_ATTRIBUTE_HIDDEN)
        return ERROR_INVALID_PARAMETER;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    FILE_BASIC_INFO current{};
    DWORD error = ERROR_SUCCESS;
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &current, sizeof(current)))
        error = GetLastError();
    else {
        FILE_BASIC_INFO changed{}; // Zero times leave concurrent timestamps unchanged.
        changed.FileAttributes = (current.FileAttributes ^ flag) & ~FILE_ATTRIBUTE_NORMAL;
        if (!changed.FileAttributes) changed.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(file, FileBasicInfo, &changed, sizeof(changed)))
            error = GetLastError();
    }
    CloseHandle(file);
    return error;
}
} // namespace pulse::app
