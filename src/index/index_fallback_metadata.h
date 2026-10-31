// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>

namespace pulse::index {

struct MftMetadataIo {
    decltype(&OpenFileById) open = &OpenFileById;
    decltype(&GetFileInformationByHandle) query = &GetFileInformationByHandle;
    decltype(&CloseHandle) close = &CloseHandle;
};

// ENUM_USN_DATA contains identity/name data, not file size or last-write time.
// Resolve each identity before admitting it to a snapshot; a failed lookup must
// fail the build rather than persist a fabricated zero-byte file.
inline bool ReadMftFallbackMetadata(HANDLE volume, uint64_t frn, bool is_dir,
                                    uint64_t& size, uint64_t& mtime,
                                    const MftMetadataIo& io = {}) {
    FILE_ID_DESCRIPTOR id{};
    id.dwSize = sizeof(id);
    id.Type = FileIdType;
    id.FileId.QuadPart = static_cast<LONGLONG>(frn);
    HANDLE file = io.open(volume, &id, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = io.query(file, &info) != FALSE;
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    io.close(file);
    const uint64_t actual = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    if (!ok || actual != frn || ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != is_dir) {
        SetLastError(error ? error : ERROR_FILE_INVALID);
        return false;
    }
    size = is_dir ? 0 : (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    mtime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    return true;
}

} // namespace pulse::index
