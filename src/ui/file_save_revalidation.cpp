// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#include "file_save_revalidation.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace pulse::ui {
namespace {
struct Metadata {
    SaveObjectIdentity identity;
    DWORD attributes = 0;
};
std::wstring Native(const std::wstring& path) {
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}
bool Cancelled(const std::function<bool()>& cancelled) { return cancelled && cancelled(); }
DWORD Query(const std::wstring& path, Metadata& metadata, bool allow_legacy_identity,
    bool* absent = nullptr, bool allow_missing_parent = false) {
    if (absent) *absent = false;
    struct Handle {
        HANDLE value;
        ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } handle{CreateFileW(Native(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (absent) *absent = error == ERROR_FILE_NOT_FOUND || (allow_missing_parent && error == ERROR_PATH_NOT_FOUND);
        return error;
    }
    FILE_BASIC_INFO basic{};
    const bool have_basic = GetFileInformationByHandleEx(handle.value, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
    DWORD error = have_basic ? ERROR_SUCCESS : GetLastError();
    if (have_basic) {
        if (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
        FILE_ID_INFO identity{};
        if (GetFileInformationByHandleEx(handle.value, FileIdInfo, &identity, sizeof(identity))) {
            metadata.attributes = basic.FileAttributes;
            metadata.identity.creation_time = basic.CreationTime.QuadPart;
            metadata.identity.volume = identity.VolumeSerialNumber;
            std::copy_n(identity.FileId.Identifier, metadata.identity.file.size(), metadata.identity.file.begin());
            return ERROR_SUCCESS;
        }
        error = GetLastError();
    }
    if (!allow_legacy_identity || (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
        error != ERROR_INVALID_FUNCTION && error != ERROR_INVALID_LEVEL)) return error;
    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!GetFileInformationByHandle(handle.value, &legacy)) return GetLastError();
    if (legacy.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_NOT_SUPPORTED;
    const auto index = (static_cast<ULONGLONG>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
    if (!index) return ERROR_NOT_SUPPORTED;
    metadata.attributes = have_basic ? basic.FileAttributes : legacy.dwFileAttributes;
    metadata.identity.creation_time = have_basic ? basic.CreationTime.QuadPart : static_cast<LONGLONG>(
        (static_cast<ULONGLONG>(legacy.ftCreationTime.dwHighDateTime) << 32) | legacy.ftCreationTime.dwLowDateTime);
    metadata.identity.volume = legacy.dwVolumeSerialNumber;
    std::memcpy(metadata.identity.file.data(), &index, sizeof(index));
    metadata.identity.legacy = true;
    return ERROR_SUCCESS;
}
DWORD Parent(const std::wstring& path, Metadata& value, bool allow_legacy_identity) {
    const DWORD error = Query(path, value, allow_legacy_identity);
    return error ? error : (value.attributes & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_SUCCESS : ERROR_DIRECTORY;
}
struct Observation {
    SaveObjectIdentity parent;
    bool parent_exists = true;
    std::optional<SaveObjectIdentity> target;
};
DWORD Observe(const std::wstring& path, const std::wstring& parent,
              const SaveObjectIdentity* expected_parent, const std::function<bool()>& cancelled, Observation& out,
              bool caller_validates_access, bool no_test, bool require_parent, bool allow_legacy_identity) {
    if (Cancelled(cancelled)) return ERROR_CANCELLED;
    Metadata first;
    DWORD error = Parent(parent, first, allow_legacy_identity);
    if (no_test && !require_parent && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
        out.parent_exists = false;
        auto ancestor = std::filesystem::path(parent).parent_path();
        while (!ancestor.empty()) {
            error = Parent(ancestor.wstring(), first, allow_legacy_identity);
            if (!error) break;
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
            const auto next = ancestor.parent_path();
            if (next == ancestor) return ERROR_PATH_NOT_FOUND;
            ancestor = next;
        }
        if (ancestor.empty()) return ERROR_PATH_NOT_FOUND;
    }
    if (error) return error;
    if (expected_parent && first.identity != *expected_parent) return ERROR_FILE_INVALID;
    if (Cancelled(cancelled)) return ERROR_CANCELLED;
    Metadata target;
    bool absent = false;
    error = Query(path, target, allow_legacy_identity, &absent, no_test && !require_parent);
    // Only a missing-path error from opening the target is absence. Even that code
    // from a metadata/identity query on an open handle remains a real failure.
    if (error && !absent) return error;
    if (!error) {
        if (target.attributes & FILE_ATTRIBUTE_DIRECTORY) return ERROR_DIRECTORY;
        if (!caller_validates_access && (target.attributes & FILE_ATTRIBUTE_READONLY)) return ERROR_ACCESS_DENIED;
        out.target = target.identity;
    }
    if (Cancelled(cancelled)) return ERROR_CANCELLED;
    if (no_test && !out.parent_exists) { out.parent = first.identity; return Cancelled(cancelled) ? ERROR_CANCELLED : ERROR_SUCCESS; }
    Metadata last;
    error = Parent(parent, last, allow_legacy_identity);
    if (error) return error;
    if (last.identity != first.identity) return ERROR_FILE_INVALID;
    if (Cancelled(cancelled)) return ERROR_CANCELLED;
    out.parent = first.identity;
    return ERROR_SUCCESS;
}
SaveRevalidationResult Failed(DWORD error) {
    SaveRevalidationResult result;
    result.error = error;
    return result;
}
}

SaveRevalidationResult CaptureSaveConfirmation(const SaveTargetRequest& request, const std::function<bool()>& cancelled) {
    if (Cancelled(cancelled)) return Failed(ERROR_CANCELLED);
    const auto resolved = ResolveSaveTarget(request);
    if (resolved.state != SaveTargetState::Resolved)
        return Failed(resolved.state == SaveTargetState::Unsupported ? ERROR_NOT_SUPPORTED : ERROR_INVALID_NAME);
    SaveConfirmationSnapshot snapshot;
    snapshot.path_ = resolved.path;
    snapshot.parent_ = std::filesystem::path(resolved.path).parent_path().wstring();
    snapshot.filter_index_ = resolved.filter_index;
    snapshot.caller_validates_access_ = request.caller_validates_access;
    snapshot.no_test_file_create_ = request.no_test_file_create;
    snapshot.require_existing_parent_ = request.require_existing_save_parent;
    snapshot.allow_legacy_identity_ = request.allow_legacy_identity;
    Observation observed;
    const DWORD error = Observe(snapshot.path_, snapshot.parent_, nullptr, cancelled, observed,
        snapshot.caller_validates_access_, snapshot.no_test_file_create_, snapshot.require_existing_parent_, snapshot.allow_legacy_identity_);
    if (error) return Failed(error);
    snapshot.parent_identity_ = observed.parent;
    snapshot.parent_exists_ = observed.parent_exists;
    snapshot.target_ = observed.target;
    SaveRevalidationResult result;
    result.error = ERROR_SUCCESS;
    result.state = snapshot.target_ ? SaveRevalidationState::ConfirmOverwrite : SaveRevalidationState::Ready;
    result.snapshot = std::move(snapshot);
    return result;
}

SaveRevalidationResult RevalidateSaveConfirmation(const SaveConfirmationSnapshot& snapshot, const std::function<bool()>& cancelled) {
    if (snapshot.path_.empty() || snapshot.parent_.empty()) return Failed(ERROR_INVALID_PARAMETER);
    Observation observed;
    const DWORD error = Observe(snapshot.path_, snapshot.parent_, snapshot.parent_exists_ ? &snapshot.parent_identity_ : nullptr,
        cancelled, observed, snapshot.caller_validates_access_, snapshot.no_test_file_create_, snapshot.require_existing_parent_, snapshot.allow_legacy_identity_);
    if (error) return Failed(error);
    SaveRevalidationResult result;
    result.error = ERROR_SUCCESS;
    result.state = observed.target && observed.target != snapshot.target_ ?
        SaveRevalidationState::ConfirmOverwrite : SaveRevalidationState::Ready;
    result.snapshot = snapshot;
    result.snapshot->target_ = observed.target;
    result.snapshot->parent_exists_ = observed.parent_exists;
    result.snapshot->parent_identity_ = observed.parent;
    return result;
}

} // namespace pulse::ui
