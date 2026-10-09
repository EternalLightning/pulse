#pragma once

#include <windows.h>
#include <cstring>
#include <string>

namespace pulse {

struct FileIdentity {
    FILE_ID_INFO value{};
    bool extended = false;

    bool operator==(const FileIdentity& other) const {
        return extended == other.extended && value.VolumeSerialNumber == other.value.VolumeSerialNumber &&
            std::memcmp(&value.FileId, &other.value.FileId, sizeof(value.FileId)) == 0;
    }
    bool Valid() const {
        bool nonzero = false;
        for (size_t i = 0; i < sizeof(value.FileId.Identifier); ++i) {
            nonzero = nonzero || value.FileId.Identifier[i] != 0;
            if (!extended && i >= sizeof(ULONGLONG) && value.FileId.Identifier[i]) return false;
        }
        return nonzero;
    }
};

inline bool ReadFileIdentity(HANDLE handle, FileIdentity& identity,
                            decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) {
    FILE_ID_INFO extended{};
    if (query(handle, FileIdInfo, &extended, sizeof(extended))) {
        identity = {extended, true};
        return true;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
        error != ERROR_INVALID_FUNCTION && error != ERROR_CALL_NOT_IMPLEMENTED) return false;

    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!GetFileInformationByHandle(handle, &legacy)) return false;
    const ULONGLONG index = (static_cast<ULONGLONG>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
    if (!index) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    identity = {};
    identity.value.VolumeSerialNumber = legacy.dwVolumeSerialNumber;
    std::memcpy(&identity.value.FileId, &index, sizeof(index));
    return true;
}

inline bool ReadPathFileIdentity(const std::wstring& path, FileIdentity& identity) {
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool read = ReadFileIdentity(handle, identity);
    const DWORD error = GetLastError();
    CloseHandle(handle);
    if (!read) SetLastError(error);
    return read;
}

} // namespace pulse
