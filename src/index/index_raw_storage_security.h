#pragma once
#include "index_transport_security.h"
#include <aclapi.h>
#include <deque>

namespace pulse::index {
// Repair explicit as well as inherited legacy AU grants on every object. Never
// follow reparse points while privileged, including at the selected raw root.
inline bool ProtectIndexRawTree(const std::wstring& root) {
    if (root.size() <= 3 || root[1] != L':' || root[2] != L'\\' || root.find(L'\0') != std::wstring::npos ||
        root.find(L"\\..") != std::wstring::npos || root.find(L"\\.\\") != std::wstring::npos) return false;
    // Opening the final component without following reparses is insufficient
    // when a privileged caller-selected ancestor is itself a junction.
    for (size_t slash = root.find(L'\\', 3);; slash = root.find(L'\\', slash + 1)) {
        const auto ancestor = slash == std::wstring::npos ? root : root.substr(0, slash);
        const auto attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (slash == std::wstring::npos) break;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
    PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    PSID owner = nullptr; BOOL owner_defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present ||
        !GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted)) { LocalFree(descriptor); return false; }
    std::deque<std::wstring> pending{root}; bool ok = true;
    while (!pending.empty() && ok) {
        auto path = std::move(pending.front()); pending.pop_front();
        transport::Handle object(CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (object.get() == INVALID_HANDLE_VALUE) { ok = false; break; }
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(object.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ok = false; break; }
        if (SetSecurityInfo(object.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION, owner, nullptr, dacl, nullptr) != ERROR_SUCCESS) { ok = false; break; }
        if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        WIN32_FIND_DATAW entry{}; HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) {
            ok = GetLastError() == ERROR_FILE_NOT_FOUND; continue;
        }
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ok = false; break; }
            const bool directory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (directory) pending.push_front(path + L"\\" + entry.cFileName);
            else pending.push_back(path + L"\\" + entry.cFileName);
        } while (FindNextFileW(find, &entry));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = false;
        FindClose(find);
    }
    LocalFree(descriptor); return ok;
}
} // namespace pulse::index
