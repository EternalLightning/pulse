#pragma once

#include "file_identity.h"
#include "../common/file_create.h"

#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
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
    explicit DestinationGuard(decltype(&GetFileInformationByHandleEx) identity_query = GetFileInformationByHandleEx)
        : identity_query_(identity_query) {}

    struct HandleCloser {
        void operator()(void* handle) const {
            if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
    };
    using Handle = std::unique_ptr<void, HandleCloser>;
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
        return true;
    }

    bool PinDirectories(const std::wstring& path, std::wstring& error) {
        for (const auto& part : Components(path)) {
            const auto found = snapshots_.find(part);
            if (found == snapshots_.end()) return Fail(part, error);
            if (!found->second.exists) continue;
            if (!found->second.directory) continue;
            if (leases_.contains(part)) continue;
            Handle handle(Open(part, false));
            Snapshot current;
            if (!handle || !Read(part, current, handle.get()) || !Same(found->second, current))
                return Fail(part, error);
            leases_.emplace(part, std::move(handle));
            if (!current.reparse.empty() && !PinDirectories(current.resolved_path, error))
                return false;
        }
        return true;
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
            found->second = std::move(created);
            leases_.emplace(part, std::move(handle));
            created_directories_.insert(part);
        }
        return true;
    }

    bool Check(const std::wstring& path, std::wstring& error) const {
        for (const auto& part : Components(path)) {
            const auto found = snapshots_.find(part);
            Snapshot current;
            if (found == snapshots_.end() || !Read(part, current) || !Same(found->second, current))
                return Fail(part, error);
        }
        return true;
    }

    Handle PinLeaf(const std::wstring& path, std::wstring& error, DWORD* code = nullptr) const {
        if (code) *code = ERROR_INVALID_DATA; // identity rejection is not a lock failure
        const auto parts = Components(path);
        if (parts.empty()) { Fail(path, error); return {}; }
        const auto found = snapshots_.find(parts.back());
        if (found == snapshots_.end()) { Fail(path, error); return {}; }
        Handle handle(Open(parts.back(), true));
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
        const auto lease = leases_.find(parts.back());
        if (lease == leases_.end() || !Check(path, error)) return false;
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
        Snapshot snapshot;
        if (!Read(parts.back(), snapshot, handle)) return Fail(path, error);
        Snapshot current;
        if (!Read(parts.back(), current) || !Same(snapshot, current)) return Fail(path, error);
        snapshots_[parts.back()] = std::move(current);
        return true;
    }

    bool DeleteOwnedLeaf(const std::wstring& path, std::wstring& error) {
        auto handle = PinLeaf(path, error);
        if (!handle || !Check(path, error)) return false;
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo,
                                       &disposition, sizeof(disposition))) return Fail(path, error);
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

    static bool RenameHandle(HANDLE handle, const std::wstring& destination) {
        const auto parts = Components(destination);
        if (parts.empty()) return false;
        const auto& name = parts.back();
        const size_t bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
        if (bytes > MAXDWORD) return false;
        std::vector<BYTE> buffer(bytes);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
        return SetFileInformationByHandle(handle, FileRenameInfo, rename,
                                          static_cast<DWORD>(buffer.size())) != FALSE;
    }

private:
    static bool Fail(const std::wstring& path, std::wstring& error) {
        error = L"目标路径身份无法确认或已变化，操作已停止：" + path;
        return false;
    }
    static HANDLE Open(const std::wstring& path, bool deletion) {
        HANDLE handle = CreateFileW(path.c_str(), deletion ? DELETE | FILE_READ_ATTRIBUTES : 0,
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
        if (!result.id.extended && owned)
            result.reference = std::shared_ptr<void>(owned.release(), HandleCloser{});
        if (!(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
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
        if (!result.resolved_id.extended)
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
        if (name.starts_with(L"\\\\?\\UNC\\")) {
            const size_t server = name.find(L'\\', 8);
            root = server == std::wstring::npos ? 0 : name.find(L'\\', server + 1);
        } else if (name.starts_with(L"\\\\" ) && !name.starts_with(L"\\\\?\\")) {
            const size_t server = name.find(L'\\', 2);
            root = server == std::wstring::npos ? 0 : name.find(L'\\', server + 1);
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
    std::map<std::wstring, Handle> leases_;
    std::set<std::wstring> created_directories_;
    std::vector<DestinationLinkImpact> impacts_;
};

} // namespace pulse::ops
