#pragma once
#include "index_transport_security.h"
#include "change_tracking.h"
#include <aclapi.h>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

namespace pulse::index {
// A response-scoped cache, never a persistent allow cache. Every new page,
// subscription refresh and feed request re-reads ACLs; security changes cannot
// resurrect a previous page's authorization decisions. Only local DOS paths
// are admitted: SYSTEM must not follow caller-selected remote/device paths.
class CallerVisibility {
public:
    explicit CallerVisibility(HANDLE token, DWORD budget_ms = 2000, bool directory_snapshot = false)
        : token_(token), deadline_(GetTickCount64() + budget_ms), directory_snapshot_(directory_snapshot) {}
    bool Incomplete() const { return incomplete_; }
    bool ValidateDirectories() {
        // A cached parent is an authorization snapshot, not a timeless allow.
        // Revalidate all snapshots before publishing total/page/aggregate; a
        // security or identity change invalidates the whole response.
        for (const auto& [path, expected] : directories_) {
            Descriptor current;
            if (!ReadDescriptor(path, true, current) || current.bytes != expected.bytes ||
                current.identity.VolumeSerialNumber != expected.identity.VolumeSerialNumber ||
                memcmp(&current.identity.FileId, &expected.identity.FileId, sizeof(FILE_ID_128)) != 0) {
                incomplete_ = true; return false;
            }
        }
        return true;
    }
    bool Visible(const std::wstring& path) {
        if (path.find(L'/') != std::wstring::npos) {
            auto normalized = path; std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
            return Visible(normalized);
        }
        if (!token_ || path.size() < 3 || path.size() > 32767 || path[1] != L':' || path[2] != L'\\' ||
            !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
            path.find(L'\0') != std::wstring::npos || path.find(L':', 2) != std::wstring::npos) return false;
        if (GetTickCount64() >= deadline_) { incomplete_ = true; return false; }
        size_t start = 3;
        if (!Access(path.substr(0, 3), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true)) return false;
        while (start < path.size()) {
            const auto slash = path.find(L'\\', start);
            const auto component = std::wstring_view(path).substr(start, slash == std::wstring::npos ? path.size() - start : slash - start);
            if (component.empty() || component == L"." || component == L"..") return false;
            if (slash == std::wstring::npos) break;
            if (!Access(path.substr(0, slash), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, true)) return false;
            start = slash + 1;
        }
        return Access(path, FILE_READ_ATTRIBUTES, false);
    }
    bool RecordVisible(const ChangeRecord& record) {
        // A deleted path cannot be re-authorized against its former descriptor.
        // Do not infer access from the current parent or disclose its old name.
        if (record.kind == ChangeKind::Deleted || record.kind == ChangeKind::MovedOut) return false;
        return Visible(record.path) && (record.old_path.empty() || Visible(record.old_path));
    }
private:
    struct Descriptor { std::vector<BYTE> bytes; bool directory = false; FILE_ID_INFO identity{}; };
    bool ReadDescriptor(const std::wstring& path, bool directory, Descriptor& descriptor) {
        transport::Handle file(CreateFileW(path.c_str(), READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) return false;
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (directory && !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))) return false;
        descriptor.directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (descriptor.directory && !GetFileInformationByHandleEx(file.get(), FileIdInfo, &descriptor.identity, sizeof(descriptor.identity))) return false;
        PSECURITY_DESCRIPTOR security = nullptr;
        if (GetSecurityInfo(file.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &security) != ERROR_SUCCESS) return false;
        const DWORD size = GetSecurityDescriptorLength(security);
        if (!size || size > 65536) { LocalFree(security); return false; }
        descriptor.bytes.assign(static_cast<BYTE*>(security), static_cast<BYTE*>(security) + size);
        LocalFree(security); return true;
    }
    bool Access(const std::wstring& path, DWORD desired, bool directory) {
        if (GetTickCount64() >= deadline_) { incomplete_ = true; return false; }
        Descriptor descriptor;
        auto cached = directories_.find(path);
        if (cached != directories_.end()) {
            // Only successfully authorized directories enter this cache.
            if (directory_snapshot_) return true;
            descriptor = cached->second;
        } else if (!ReadDescriptor(path, directory, descriptor)) return false;
        if (descriptor.directory) desired |= FILE_LIST_DIRECTORY;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        std::vector<BYTE> privileges(sizeof(PRIVILEGE_SET) + 16 * sizeof(LUID_AND_ATTRIBUTES));
        DWORD privilege_size = static_cast<DWORD>(privileges.size()), granted = 0; BOOL allowed = FALSE;
        const auto security = reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.bytes.data());
        if (!AccessCheck(security, token_, desired, &mapping, reinterpret_cast<PRIVILEGE_SET*>(privileges.data()),
            &privilege_size, &granted, &allowed) || !allowed || (granted & desired) != desired) return false;
        // The kernel open also enforces mandatory integrity policy and refreshed
        // restricted-token ACLs. Cached descriptors can never grant stale access.
        if (!ImpersonateLoggedOnUser(token_)) return false;
        HANDLE visible = CreateFileW(path.c_str(), desired,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        transport::RevertOrFailFast();
        transport::Handle proof(visible);
        FILE_ATTRIBUTE_TAG_INFO tag{};
        const bool allowed_now = proof.get() != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandleEx(proof.get(), FileAttributeTagInfo, &tag, sizeof(tag)) &&
            !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (!directory || (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY));
        if (allowed_now && descriptor.directory && cached == directories_.end() &&
            directories_.size() < 1024 && cached_bytes_ + descriptor.bytes.size() <= 1024 * 1024) {
            cached_bytes_ += descriptor.bytes.size(); directories_.emplace(path, std::move(descriptor));
        }
        return allowed_now;
    }
    HANDLE token_ = nullptr;
    ULONGLONG deadline_;
    bool incomplete_ = false;
    bool directory_snapshot_ = false;
    size_t cached_bytes_ = 0;
    std::map<std::wstring, Descriptor> directories_;
};
} // namespace pulse::index
