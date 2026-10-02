#include "deletion_identity.h"
#include "../common/path_utils.h"
#include <cstring>

namespace pulse::shell {
namespace {
bool ReadIdentity(std::wstring_view path, FILE_ID_INFO& identity) {
    std::wstring extended;
    if (path.starts_with(L"\\\\")) extended = L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    else if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\')
        extended = L"\\\\?\\" + std::wstring(path);
    else return false;
    const HANDLE file = CreateFileW(extended.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool read = GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)) != FALSE;
    CloseHandle(file);
    return read;
}
}

bool IsSameDeletionItem(std::wstring_view requested, std::wstring_view resolved) {
    const auto requested_path = path::StripExtendedPathPrefix(requested);
    const auto resolved_path = path::StripExtendedPathPrefix(resolved);
    if (requested_path == resolved_path) return true;
    if (!path::EqualInsensitive(requested_path, resolved_path)) return false;
    // Shell can canonicalize casing (notably $RECYCLE.BIN). Case-insensitive
    // equality alone could authorize a different file in a case-sensitive directory.
    FILE_ID_INFO requested_id{}, resolved_id{};
    return ReadIdentity(requested_path, requested_id) && ReadIdentity(resolved_path, resolved_id) &&
        requested_id.VolumeSerialNumber == resolved_id.VolumeSerialNumber &&
        std::memcmp(&requested_id.FileId, &resolved_id.FileId, sizeof(requested_id.FileId)) == 0;
}
}
