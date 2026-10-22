#include "deletion_identity.h"
#include "../common/file_identity.h"
#include "../common/path_utils.h"

namespace pulse::shell {
namespace {
bool ReadIdentity(std::wstring_view path, FileIdentity& identity,
    decltype(&GetFileInformationByHandleEx) query) {
    std::wstring extended;
    if (path.starts_with(L"\\\\")) extended = L"\\\\?\\UNC\\" + std::wstring(path.substr(2));
    else if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\')
        extended = L"\\\\?\\" + std::wstring(path);
    else return false;
    const HANDLE file = CreateFileW(extended.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool read = ReadFileIdentity(file, identity, query) && identity.Valid();
    CloseHandle(file);
    return read;
}
}

bool DeletionReference::Capture(const std::wstring& path, const FileIdentity& expected,
    decltype(&GetFileInformationByHandleEx) query) {
    handle_.reset();
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::shared_ptr<void> reference(handle, [](void* value) { CloseHandle(value); });
    FileIdentity current;
    if (!expected.Valid() || !ReadFileIdentity(handle, current, query) || current != expected) return false;
    handle_ = std::move(reference);
    return true;
}

bool DeletionReference::Matches(const std::wstring& path,
    decltype(&GetFileInformationByHandleEx) query) const {
    if (!handle_) return false;
    const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::shared_ptr<void> reference(handle, [](void* value) { CloseHandle(value); });
    FileIdentity retained, current;
    // RaiDrive may replace a provisional index when Shell enumerates the item.
    // Compare the retained object with the current path, never adopt a new path's ID.
    return ReadFileIdentity(handle_.get(), retained, query) && retained.Valid() &&
        ReadFileIdentity(handle, current, query) && retained == current;
}

bool IsSameDeletionItem(std::wstring_view requested, std::wstring_view resolved,
    decltype(&GetFileInformationByHandleEx) query) {
    const auto requested_path = path::StripExtendedPathPrefix(requested);
    const auto resolved_path = path::StripExtendedPathPrefix(resolved);
    if (requested_path == resolved_path) return true;
    if (!path::EqualInsensitive(requested_path, resolved_path)) return false;
    // Shell can canonicalize casing (notably $RECYCLE.BIN). Case-insensitive
    // equality alone could authorize a different file in a case-sensitive directory.
    // Redirectors may expose only the legacy file index. Use the same identity
    // contract as admission instead of turning an unsupported query into denial.
    FileIdentity requested_id{}, resolved_id{};
    return ReadIdentity(requested_path, requested_id, query) && ReadIdentity(resolved_path, resolved_id, query) &&
        requested_id == resolved_id;
}
}
