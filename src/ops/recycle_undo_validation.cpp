// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "recycle_undo_validation.h"
#include "destination_guard.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_enum.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include <cstring>
#include <cwctype>

namespace pulse::ops {
namespace {
bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    return a.VolumeSerialNumber == b.VolumeSerialNumber &&
        std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}
bool ReadId(const std::wstring& path, FILE_ID_INFO& id) {
    HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool ok = GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)) != FALSE;
    CloseHandle(handle);
    return ok;
}
}
bool IsCurrentUserRecyclePayload(const std::wstring& original, const std::wstring& payload) {
    const auto source = path::StripExtendedPathPrefix(original), recycled = path::StripExtendedPathPrefix(payload);
    if (source.size() < 3 || recycled.size() < 3 || source[1] != L':' || recycled[1] != L':' ||
        towupper(source[0]) != towupper(recycled[0])) return false;
    const auto slash = recycled.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const auto leaf = recycled.substr(slash + 1);
    const auto sid = CurrentUserSidString();
    const auto parent = recycled.substr(0, slash);
    const auto expected = recycled.substr(0, 2) + L"\\$Recycle.Bin\\" + sid;
    return !sid.empty() && leaf.size() > 2 && leaf[0] == L'$' && (leaf[1] == L'R' || leaf[1] == L'r') &&
        CompareStringOrdinal(parent.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool ValidateRecycleUndoPair(const std::wstring& original, const std::wstring& payload,
    RecycleUndoIdentity& identity, const RecycleUndoIdentity* expected, bool require_empty_destination) {
    if (require_empty_destination) {
        if (GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        const DWORD absent = GetLastError();
        if (absent != ERROR_FILE_NOT_FOUND && absent != ERROR_PATH_NOT_FOUND) return false;
    }
    const auto index = fs::RecycleIndexPath(payload);
    fs::RecycleItem recorded;
    FILE_ID_INFO index_before{}, payload_after{};
    if (index.empty() || !ReadId(index, index_before) || !ReadId(payload, identity.payload) ||
        !fs::ReadRecycleIndex(index, recorded) ||
        fs::NormalizePath(recorded.original_path) != fs::NormalizePath(original) ||
        !ReadId(index, identity.index) || !SameId(index_before, identity.index) ||
        !ReadId(payload, payload_after) || !SameId(identity.payload, payload_after)) return false;
    return !expected || (SameId(identity.payload, expected->payload) && SameId(identity.index, expected->index));
}

bool RestoreRecyclePair(const std::wstring& original, const std::wstring& payload,
    const std::wstring& destination, const RecycleUndoIdentity& expected, DWORD& error,
    DestinationGuard* continuation_guard) {
    error = ERROR_SUCCESS;
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    // Deny deletion/replacement while validating and renaming these exact files.
    Handle content{CreateFileW(payload.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (content.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    const auto index_path = fs::RecycleIndexPath(payload);
    Handle index{CreateFileW(index_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (index.value == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    FILE_ID_INFO content_id{}, index_id{};
    RecycleUndoIdentity verified;
    if (!GetFileInformationByHandleEx(content.value, FileIdInfo, &content_id, sizeof(content_id)) ||
        !GetFileInformationByHandleEx(index.value, FileIdInfo, &index_id, sizeof(index_id))) {
        error = GetLastError(); return false;
    }
    if (!SameId(content_id, expected.payload) || !SameId(index_id, expected.index) ||
        !ValidateRecycleUndoPair(original, payload, verified, &expected, false)) {
        error = ERROR_INVALID_DATA; return false;
    }
    if (destination.empty() || destination.find(L'\0') != std::wstring::npos) {
        error = ERROR_INVALID_NAME; return false;
    }
    // Reuse the rename path used by ordinary rename and transfer commit. It
    // normalizes the Win32 path and supplies the full zero-terminated buffer
    // expected by SetFileInformationByHandle, while keeping ReplaceIfExists false.
    if (!(continuation_guard ? continuation_guard->RenameLeaf(content.value, destination)
                             : DestinationGuard::RenameHandle(content.value, destination))) {
        error = GetLastError(); return false;
    }
    // Restore authorizes a content move, not irreversible metadata deletion.
    // Enumeration ignores this index after the exact payload is moved away.
    return true;
}
}
