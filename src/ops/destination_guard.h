// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "file_identity.h"
#include "file_object_reference.h"
#include "../common/file_create.h"
#include "../fs/cloud_state.h"

#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <vector>

namespace pulse::ops {

enum class DestinationLinkKind { DirectorySymbolicLink, Junction, FileSymbolicLink, HardLink };

struct DestinationLinkImpact {
    DestinationLinkKind kind = DestinationLinkKind::HardLink;
    std::wstring path;
    std::wstring resolved_path;
    DWORD link_count = 0;
};

// Worker-only. A confirmation authorizes a snapshot, not a path string. Directory
// leases deny both deletion and reparse writes, including the resolved directory.
class DestinationGuard {
public:
    struct HandleCloser {
        void operator()(void* handle) const {
            if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    };
    using Handle = std::unique_ptr<void, HandleCloser>;
    struct DirectoryLeases {
        std::map<std::wstring, Handle> handles;
        std::set<std::wstring> commit_parents;
        std::function<void(const std::wstring&)> before_relative_rename; // regression gate, empty in production
    };
    explicit DestinationGuard(decltype(&GetFileInformationByHandleEx) identity_query = GetFileInformationByHandleEx,
        std::shared_ptr<DirectoryLeases> directory_leases = {})
        : identity_query_(identity_query), leases_(directory_leases ? std::move(directory_leases)
            : std::make_shared<DirectoryLeases>()) {}
    std::shared_ptr<DirectoryLeases> SharedDirectoryLeases() const { return leases_; }
#ifdef PULSE_DESTINATION_GUARD_TESTING
    static std::vector<std::wstring> ComponentsForTest(const std::wstring& path) { return Components(path); }
#endif
    struct Snapshot {
        bool exists = false;
        bool directory = false;
        DWORD error_code = ERROR_INVALID_DATA; // Read failure survives handle cleanup
        FileIdentity id{};
        std::shared_ptr<void> reference;
        DWORD links = 0;
        std::vector<BYTE> reparse;
        FileIdentity resolved_id{};
        std::shared_ptr<void> resolved_reference;
        std::wstring resolved_path;
    };

    struct ElevationSnapshot {
        std::wstring path;
        Snapshot snapshot;
        uint64_t reference_handle = 0;
        uint64_t resolved_reference_handle = 0;
    };

    std::vector<ElevationSnapshot> ExportForElevation() const {
        std::vector<ElevationSnapshot> result;
        result.reserve(snapshots_.size());
        for (const auto& [path, snapshot] : snapshots_) {
            ElevationSnapshot entry{path, snapshot,
                reinterpret_cast<uint64_t>(snapshot.reference.get()),
                reinterpret_cast<uint64_t>(snapshot.resolved_reference.get())};
            entry.snapshot.reference.reset();
            entry.snapshot.resolved_reference.reset();
            result.push_back(std::move(entry));
        }
        return result;
    }

    bool ImportForElevation(const std::vector<ElevationSnapshot>& entries,
        HANDLE parent_process, std::wstring& error) {
        if (!snapshots_.empty() || entries.size() > 65536 || !parent_process) return false;
        for (const auto& entry : entries) {
            const auto parts = Components(entry.path);
            if (parts.empty() || parts.back() != entry.path || snapshots_.contains(entry.path))
                return Fail(entry.path, error);
            Snapshot snapshot = entry.snapshot;
            const auto duplicate = [&](uint64_t value, const FileIdentity& expected,
                std::shared_ptr<void>& reference) {
                if (!value) return !expected.Valid();
                HANDLE raw = nullptr;
                if (!DuplicateHandle(parent_process, reinterpret_cast<HANDLE>(value),
                    GetCurrentProcess(), &raw, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
                reference = std::shared_ptr<void>(raw, HandleCloser{});
                FileIdentity identity;
                return ReadFileIdentity(raw, identity, identity_query_) && identity == expected;
            };
            if (snapshot.exists && (!snapshot.id.Valid() ||
                !duplicate(entry.reference_handle, snapshot.id, snapshot.reference)))
                return Fail(entry.path, error);
            if (!snapshot.reparse.empty() && (!snapshot.resolved_id.Valid() ||
                !duplicate(entry.resolved_reference_handle, snapshot.resolved_id, snapshot.resolved_reference)))
                return Fail(entry.path, error);
            Snapshot current;
            if (!Read(entry.path, current) || !Same(snapshot, current)) return Fail(entry.path, error);
            snapshots_.emplace(entry.path, std::move(snapshot));
        }
        return true;
    }

    bool Capture(const std::wstring& path, std::wstring& error) {
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        for (const auto& part : parts) {
            if (snapshots_.contains(part)) continue;
            Snapshot snapshot;
            if (!Read(part, snapshot)) return Fail(part, error);
            if (snapshot.exists && !snapshot.reparse.empty()) {
                DWORD tag = 0;
                std::memcpy(&tag, snapshot.reparse.data(), sizeof(tag));
                if (tag != IO_REPARSE_TAG_SYMLINK && tag != IO_REPARSE_TAG_MOUNT_POINT)
                    return Fail(part, error);
                impacts_.push_back({snapshot.directory
                        ? (tag == IO_REPARSE_TAG_MOUNT_POINT ? DestinationLinkKind::Junction
                                                           : DestinationLinkKind::DirectorySymbolicLink)
                        : DestinationLinkKind::FileSymbolicLink,
                    part, snapshot.resolved_path, snapshot.links});
            } else if (snapshot.exists && !snapshot.directory && snapshot.links > 1) {
                impacts_.push_back({DestinationLinkKind::HardLink, part, {}, snapshot.links});
            }
            const std::wstring resolved_path = snapshot.directory ? snapshot.resolved_path : L"";
            snapshots_.emplace(part, std::move(snapshot));
            if (!resolved_path.empty() && !Capture(resolved_path, error)) return false;
        }
        return true;
    }

    const std::vector<DestinationLinkImpact>& Impacts() const { return impacts_; }

    bool CapturedIdentity(const std::wstring& path, FileIdentity& identity) const {
        const auto parts = Components(path);
        if (parts.empty()) return false;
        const auto found = snapshots_.find(parts.back());
        if (found == snapshots_.end() || !found->second.exists) return false;
        identity = found->second.id;
        if (found->second.reference && !ReadFileIdentity(found->second.reference.get(), identity, identity_query_)) {
            identity = {};
            return false;
        }
        return true;
    }

    bool CapturedReference(const std::wstring& path, FileObjectReference& reference) const {
        reference = {};
        const auto parts = Components(path);
        if (parts.empty()) return false;
        const auto found = snapshots_.find(parts.back());
        std::wstring error;
        if (found == snapshots_.end() || !found->second.exists || !found->second.reference ||
            !Check(path, error)) return false;
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), found->second.reference.get(), GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
        reference.handle = std::make_shared<FileObjectReference::Lease>(duplicate);
        if (!ReadFileIdentity(duplicate, reference.identity, identity_query_)) {
            reference = {};
            return false;
        }
        return true;
    }

    void RetireObjectReference(const FileIdentity& removed) {
        for (auto& entry : snapshots_) {
            auto& snapshot = entry.second;
            const auto retire = [&](std::shared_ptr<void>& reference, FileIdentity& captured) {
                FileIdentity live;
                if (reference && ReadFileIdentity(reference.get(), live, identity_query_) && live == removed) {
                    reference.reset();
                    captured = {}; // Preserve a refusal; never readmit the pathname on retry.
                }
            };
            retire(snapshot.reference, snapshot.id);
            retire(snapshot.resolved_reference, snapshot.resolved_id);
        }
    }

    bool PinPlainDirectory(const std::wstring& path, const FileIdentity& identity, std::wstring& error) {
        if (!Capture(path, error)) return false;
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        const auto found = snapshots_.find(parts.back());
        if (found == snapshots_.end() || !found->second.exists || !found->second.directory ||
            !found->second.reparse.empty() || found->second.id != identity) return Fail(path, error);
        return PinDirectories(path, error);
    }

    bool PinDirectories(const std::wstring& path, std::wstring& error) {
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        for (const auto& part : parts) {
            const auto found = snapshots_.find(part);
            if (found == snapshots_.end()) return Fail(part, error);
            if (!found->second.exists) continue;
            if (!found->second.directory) continue;
            if (leases_->handles.contains(part) && !leases_->commit_parents.contains(part)) continue;
            Handle handle(Open(part, false, created_directories_.contains(part) ? FILE_WRITE_ATTRIBUTES : 0));
            Snapshot current;
            if (!handle || !Read(part, current, handle.get()) || !Same(found->second, current))
                return Fail(part, error);
            leases_->handles[part] = std::move(handle);
            leases_->commit_parents.erase(part);
            if (!current.reparse.empty() && !PinDirectories(current.resolved_path, error))
                return false;
        }
        return true;
    }

    // Release our directory before mutating that leaf, retaining its ancestors
    // and the snapshot used to validate any later admission or retry.
    void ReleaseDirectory(const std::wstring& path) {
        const auto parts = Components(path);
        if (!parts.empty()) {
            leases_->handles.erase(parts.back());
            leases_->commit_parents.erase(parts.back());
        }
    }

    void ReleaseDirectories() { leases_->handles.clear(); leases_->commit_parents.clear(); }

    void ForgetLeaf(const std::wstring& path) {
        const auto parts = Components(path);
        if (parts.empty()) return;
        ReleaseDirectory(path);
        snapshots_.erase(parts.back());
    }

    bool EnsureDirectories(const std::wstring& path, std::wstring& error) {
        if (!PinDirectories(path, error)) return false;
        for (const auto& part : Components(path)) {
            auto found = snapshots_.find(part);
            if (found == snapshots_.end()) return Fail(part, error);
            if (found->second.exists) {
                if (!found->second.directory) return Fail(part, error);
                continue;
            }
            // FILE_CREATE returns our directory handle atomically; no create /
            // reopen gap can adopt an externally swapped link or directory.
            Handle handle = CreateDirectoryLeaf(part, false, error);
            Snapshot created;
            if (!handle || !Read(part, created, handle.get()) || !created.directory ||
                !created.reparse.empty()) return Fail(part, error);
            // Keep a permissively shared reference while the creation lease
            // proves the path. Virtual providers may update its ID after writes.
            Snapshot current;
            if (!Read(part, current) || !Same(created, current)) return Fail(part, error);
            found->second = std::move(current);
            leases_->handles.emplace(part, std::move(handle));
            created_directories_.insert(part);
        }
        return true;
    }

    bool Check(const std::wstring& path, std::wstring& error) const {
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        for (const auto& part : parts) {
            const auto found = snapshots_.find(part);
            Snapshot current;
            if (found == snapshots_.end() || !Read(part, current) || !Same(found->second, current))
                return Fail(part, error);
        }
        return true;
    }

    Handle PinLeaf(const std::wstring& path, std::wstring& error, DWORD* code = nullptr,
                   DWORD extra_access = 0) const {
        if (code) *code = ERROR_INVALID_DATA; // identity rejection is not a lock failure
        const auto parts = Components(path);
        if (parts.empty()) { Fail(path, error); return {}; }
        const auto found = snapshots_.find(parts.back());
        if (found == snapshots_.end()) { Fail(path, error); return {}; }
        Handle handle(Open(parts.back(), true, extra_access));
        if (!handle) {
            const DWORD saved = GetLastError();
            if (code) *code = saved;
            Fail(path, error);
            return {};
        }
        Snapshot current;
        if (!Read(parts.back(), current, handle.get())) {
            if (code) *code = current.error_code;
            Fail(path, error);
            return {};
        }
        if (!Same(found->second, current)) { Fail(path, error); return {}; }
        if (code) *code = ERROR_SUCCESS;
        return handle;
    }

    bool RefreshOwnedLeaf(const std::wstring& path, std::wstring& error) {
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        Snapshot snapshot;
        if (!Read(parts.back(), snapshot)) return Fail(path, error);
        snapshots_[parts.back()] = std::move(snapshot);
        return true;
    }

    bool SetDirectoryMetadata(const std::wstring& path, const FILETIME& created,
                              const FILETIME& accessed, const FILETIME& modified,
                              DWORD source_attributes, std::wstring& error) {
        const auto parts = Components(path);
        if (parts.empty()) return Fail(path, error);
        const auto lease = leases_->handles.find(parts.back());
        if (lease == leases_->handles.end() || !Check(path, error)) return false;
        if (!created_directories_.contains(parts.back())) {
            // Do not rewrite the metadata of a preexisting merge directory (or a
            // linked target). Its lease deliberately denies attribute/reparse writes.
            return true;
        }
        HANDLE handle = lease->second.get();
        if (!SetFileTime(handle, &created, &accessed, &modified)) return Fail(path, error);
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return Fail(path, error);
        constexpr DWORD copied = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        basic.FileAttributes = (basic.FileAttributes & ~copied) | (source_attributes & copied);
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) return Fail(path, error);
        return true;
    }

    bool AdoptCopyLeaf(const std::wstring& path, HANDLE owned, std::wstring& error) {
        const auto parts = Components(path);
        if (!owned || parts.empty()) return Fail(path, error);
        Snapshot snapshot, current;
        if (!Read(parts.back(), snapshot, owned) || !Read(parts.back(), current) ||
            !snapshot.exists || snapshot.directory || !snapshot.reparse.empty() || !Same(snapshot, current))
            return Fail(path, error);
        snapshots_[parts.back()] = std::move(current);
        return true;
    }

    bool AdoptCreatedLeaf(const std::wstring& path, HANDLE handle, std::wstring& error) {
        const auto parts = Components(path);
        if (!handle || parts.empty()) return Fail(path, error);
        if (parts.size() < 2 || !PinDirectories(parts[parts.size() - 2], error)) return false;
        Snapshot snapshot;
        if (!Read(parts.back(), snapshot, handle)) return Fail(path, error);
        Snapshot current;
        if (!Read(parts.back(), current) || !Same(snapshot, current)) return Fail(path, error);
        snapshots_[parts.back()] = std::move(current);
        return true;
    }

    bool DeleteOwnedLeaf(const std::wstring& path, std::wstring& error, Handle handle = {}) {
        if (!handle) handle = PinLeaf(path, error);
        if (!handle || !Check(path, error)) return false;
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo,
                                       &disposition, sizeof(disposition))) return Fail(path, error);
        ForgetLeaf(path);
        handle.reset();
        return RefreshOwnedLeaf(path, error);
    }

    static Handle CreateDirectoryLeaf(const std::wstring& path, bool reparse,
                                      std::wstring& error) {
        const auto parts = Components(path);
        if (parts.empty()) { Fail(path, error); return {}; }
        HANDLE handle = pulse::CreateDirectoryHandle(parts.back(), reparse);
        if (!handle) { Fail(path, error); return {}; }
        return Handle(handle);
    }

    bool RenameLeaf(HANDLE handle, const std::wstring& destination) const {
        const auto parts = Components(destination);
        if (parts.size() < 2) { SetLastError(ERROR_INVALID_NAME); return false; }
        const auto& parent_path = parts[parts.size() - 2];
        const auto snapshot = snapshots_.find(parent_path);
        if (!leases_->handles.contains(parent_path) || snapshot == snapshots_.end()) {
            SetLastError(ERROR_INVALID_HANDLE); return false;
        }
        // Rename opens the target parent for adding a name. Permit that access
        // only on this pinned object and commit relative to its handle. Ancestors
        // retain their strict leases, and the next path admission repins this leaf.
        leases_->handles.erase(parent_path);
        const DWORD access = FILE_LIST_DIRECTORY | (created_directories_.contains(parent_path) ? FILE_WRITE_ATTRIBUTES : 0);
        Handle parent(CreateFileW(parent_path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        Snapshot current;
        if (parent.get() == INVALID_HANDLE_VALUE) { parent.release(); return false; }
        if (!parent || !Read(parent_path, current, parent.get()) || !Same(snapshot->second, current)) {
            SetLastError(ERROR_FILE_INVALID); return false;
        }
        const auto raw_parent = parent.get();
        leases_->handles[parent_path] = std::move(parent);
        leases_->commit_parents.insert(parent_path);
        if (leases_->before_relative_rename) leases_->before_relative_rename(destination);
        return RenameHandle(handle, destination, raw_parent);
    }

    static bool RenameHandle(HANDLE handle, const std::wstring& destination, HANDLE parent = nullptr) {
        const auto parts = Components(destination);
        if (parts.empty()) return false;
        const std::wstring name = parent ? std::filesystem::path(parts.back()).filename().wstring() : parts.back();
        const size_t bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
        if (bytes > MAXDWORD) return false;
        std::vector<BYTE> buffer(bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = parent;
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
        if (parent) {
            using Rename = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
            using StatusError = ULONG (NTAPI*)(NTSTATUS);
            static const auto rename_file = reinterpret_cast<Rename>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
            static const auto status_error = reinterpret_cast<StatusError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            if (!rename_file) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
            IO_STATUS_BLOCK status{};
            constexpr auto rename_information = static_cast<FILE_INFORMATION_CLASS>(10);
            const NTSTATUS result = rename_file(handle, &status, rename, static_cast<ULONG>(buffer.size()), rename_information);
            if (result < 0) { SetLastError(status_error ? status_error(result) : ERROR_ACCESS_DENIED); return false; }
            return true;
        }
        return SetFileInformationByHandle(handle, FileRenameInfo, rename,
                                          static_cast<DWORD>(buffer.size())) != FALSE;
    }

private:
    static bool Fail(const std::wstring& path, std::wstring& error) {
        error = L"目标路径身份无法确认或已变化，操作已停止：" + path;
        return false;
    }
    static HANDLE Open(const std::wstring& path, bool deletion, DWORD extra_access = 0) {
        HANDLE handle = CreateFileW(path.c_str(), (deletion ? DELETE | FILE_READ_ATTRIBUTES : FILE_LIST_DIRECTORY) | extra_access,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }
    static bool SameId(const FileIdentity& left, const FileIdentity& right) {
        return left == right;
    }
    bool Same(const Snapshot& left, const Snapshot& right) const {
        auto left_id = left.id, right_id = right.id;
        auto left_resolved = left.resolved_id, right_resolved = right.resolved_id;
        // Some virtual filesystems expose an index tied to the open object.
        // Retain that object and compare live identities, including after rename.
        if ((left.reference && !ReadFileIdentity(left.reference.get(), left_id, identity_query_)) ||
            (right.reference && !ReadFileIdentity(right.reference.get(), right_id, identity_query_)) ||
            (left.resolved_reference && !ReadFileIdentity(left.resolved_reference.get(), left_resolved, identity_query_)) ||
            (right.resolved_reference && !ReadFileIdentity(right.resolved_reference.get(), right_resolved, identity_query_))) return false;
        return left.exists == right.exists && (!left.exists ||
            (left.directory == right.directory && SameId(left_id, right_id) &&
             left.links == right.links && left.reparse == right.reparse &&
             (left.reparse.empty() || (SameId(left_resolved, right_resolved) &&
                                      left.resolved_path == right.resolved_path))));
    }
    bool Read(const std::wstring& path, Snapshot& result, HANDLE pinned = nullptr) const {
        Handle owned;
        HANDLE handle = pinned;
        if (!handle) {
            handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) {
                const DWORD code = GetLastError();
                result.error_code = code;
                return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
            }
            owned.reset(handle);
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        FILE_STANDARD_INFO standard{};
        if (!ReadFileIdentity(handle, result.id, identity_query_) ||
            !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) {
            result.error_code = GetLastError();
            return false;
        }
        result.exists = true;
        result.directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        result.links = result.directory ? 0 : standard.NumberOfLinks;
        if (owned)
            result.reference = std::shared_ptr<void>(owned.release(), HandleCloser{});
        if (!(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        // A Cloud Files tag is not a redirected name. Its provider payload can
        // change during hydration; object identity and strict directory leases
        // still protect this path. Every other reparse keeps its exact checks.
        if (fs::IsCloudReparseTag(attributes.ReparseTag)) return true;
        result.reparse.resize(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
        DWORD bytes = 0;
        if (!DeviceIoControl(handle, FSCTL_GET_REPARSE_POINT, nullptr, 0, result.reparse.data(),
                static_cast<DWORD>(result.reparse.size()), &bytes, nullptr)) {
            result.error_code = GetLastError();
            return false;
        }
        if (bytes < sizeof(DWORD)) return false;
        result.reparse.resize(bytes);
        Handle resolved(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (resolved.get() == INVALID_HANDLE_VALUE) {
            result.error_code = GetLastError();
            resolved.release();
            return false;
        }
        if (!resolved || !ReadFileIdentity(resolved.get(), result.resolved_id, identity_query_)) {
            result.error_code = GetLastError();
            return false;
        }
        std::vector<wchar_t> name(32768);
        const DWORD length = GetFinalPathNameByHandleW(resolved.get(), name.data(),
                                                     static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED);
        if (!length || length >= name.size()) {
            result.error_code = length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            return false;
        }
        result.resolved_path.assign(name.data(), length);
        if (resolved)
            result.resolved_reference = std::shared_ptr<void>(resolved.release(), HandleCloser{});
        return true;
    }
    static std::vector<std::wstring> Components(const std::wstring& path) {
        if (path.empty()) return {};
        std::vector<wchar_t> full(32768);
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (!length || length >= full.size()) return {};
        std::wstring name = std::filesystem::path(std::wstring(full.data(), length)).lexically_normal().wstring();
        std::replace(name.begin(), name.end(), L'/', L'\\');
        size_t root = 0;
        if (name.starts_with(L"\\\\?\\UNC\\") ||
            (name.starts_with(L"\\\\") && !name.starts_with(L"\\\\?\\"))) {
            const size_t begin = name.starts_with(L"\\\\?\\UNC\\") ? 8 : 2;
            const size_t server = name.find(L'\\', begin);
            if (server == std::wstring::npos || server == begin || server + 1 == name.size()) return {};
            root = name.find(L'\\', server + 1);
            if (root == server + 1) return {};
            if (root == std::wstring::npos) {
                root = name.size();
                name.push_back(L'\\');
            }
        } else {
            const size_t drive = name.starts_with(L"\\\\?\\") ? 4 : 0;
            if (name.size() >= drive + 3 && name[drive + 1] == L':' && name[drive + 2] == L'\\') root = drive + 2;
        }
        if (!root || root == std::wstring::npos) return {};
        std::vector<std::wstring> result{name.substr(0, root + 1)};
        size_t pos = root + 1;
        while (pos < name.size()) {
            const size_t end = name.find(L'\\', pos);
            const size_t count = end == std::wstring::npos ? name.size() : end;
            if (count > pos) result.push_back(name.substr(0, count));
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
        return result;
    }

    decltype(&GetFileInformationByHandleEx) identity_query_;
    std::map<std::wstring, Snapshot> snapshots_;
    std::shared_ptr<DirectoryLeases> leases_;
    std::set<std::wstring> created_directories_;
    std::vector<DestinationLinkImpact> impacts_;
};

} // namespace pulse::ops
