// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/localization.h"
// details_meta.cpp — See details_meta.h. No COM needed (advapi32/win32 only).
#include "details_meta.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <vector>
#include <winternl.h>
#include <atomic>

namespace pulse::app {
namespace {

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

std::wstring LookupSid(PSID sid) {
    if (!sid) return {};
    wchar_t name[256]{}, domain[256]{};
    DWORD name_len = static_cast<DWORD>(std::size(name));
    DWORD domain_len = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use{};
    if (!LookupAccountSidW(nullptr, sid, name, &name_len, domain, &domain_len, &use))
        return {};
    if (domain_len > 0) return std::wstring(domain) + L"\\" + name;
    return name;
}

void FetchOwner(HANDLE file, DetailsMeta& out) {
    PSID sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT,
                              OWNER_SECURITY_INFORMATION, &sid, nullptr, nullptr,
                              nullptr, &sd) == ERROR_SUCCESS) {
        out.owner = LookupSid(sid);
        if (sd) LocalFree(sd);
    }
}

void FetchPermissions(const std::wstring& path, bool is_dir, DetailsMeta& out) {
    HANDLE file = CreateFileW(path.c_str(), MAXIMUM_ALLOWED,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto query = module ? reinterpret_cast<decltype(&NtQueryObject)>(
        GetProcAddress(module, "NtQueryObject")) : nullptr;
    PUBLIC_OBJECT_BASIC_INFORMATION basic{};
    if (query && query(file, ObjectBasicInformation, &basic, sizeof(basic), nullptr) >= 0)
        out.permissions = FormatAccessMask(basic.GrantedAccess, is_dir);
    CloseHandle(file);
}

void FetchVolume(const std::wstring& shell_path, DetailsMeta& out) {
    wchar_t root[MAX_PATH]{};
    if (!GetVolumePathNameW(shell_path.c_str(), root, ARRAYSIZE(root))) return;
    wchar_t volume_name[MAX_PATH]{}, fs_name[MAX_PATH]{};
    if (GetVolumeInformationW(root, volume_name, ARRAYSIZE(volume_name), nullptr,
                              nullptr, nullptr, fs_name, ARRAYSIZE(fs_name))) {
        out.file_system = fs_name;
        std::wstring letter = root;
        while (!letter.empty() && letter.back() == L'\\') letter.pop_back();
        out.drive = volume_name[0] ? std::wstring(volume_name) + L" (" + letter + L")"
                                   : letter;
    }
    ULARGE_INTEGER free_avail{}, total{};
    if (GetDiskFreeSpaceExW(root, &free_avail, &total, nullptr) && total.QuadPart > 0) {
        const uint64_t pct = free_avail.QuadPart * 100 / total.QuadPart;
        wchar_t text[96];
        swprintf_s(text, L"%s / %s (%llu%%)",
                   pulse::format::ByteSize(free_avail.QuadPart).c_str(),
                   pulse::format::ByteSize(total.QuadPart).c_str(),
                   static_cast<unsigned long long>(pct));
        out.free_space = text;
    }
}

} // namespace

std::wstring FormatAccessMask(unsigned int mask, bool is_dir) {
    if ((mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS) return pulse::l10n::Get(pulse::l10n::StringId::PermFull).c_str();
    // winnt.h has FILE_GENERIC_READ/WRITE/EXECUTE but no FILE_GENERIC_MODIFY.
    const bool read = (mask & FILE_GENERIC_READ) == FILE_GENERIC_READ;
    const bool write = (mask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE;
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += pulse::l10n::Get(pulse::l10n::StringId::ListSeparator);
        out += part;
    };
    const bool execute = (mask & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE;
    const bool modify = read && write && execute && (mask & DELETE);
    if (modify) add(pulse::l10n::Get(pulse::l10n::StringId::PermModify).c_str());
    if (read && execute) add(pulse::l10n::Get(pulse::l10n::StringId::PermReadExecute).c_str());
    if (is_dir && (mask & FILE_LIST_DIRECTORY) == FILE_LIST_DIRECTORY)
        add(pulse::l10n::Get(pulse::l10n::StringId::PermList).c_str());
    if (read && !modify) add(pulse::l10n::Get(pulse::l10n::StringId::PermRead).c_str());
    if (write && !modify) add(pulse::l10n::Get(pulse::l10n::StringId::PermWrite).c_str());
    if (mask & DELETE) add(pulse::l10n::Get(pulse::l10n::StringId::Delete).c_str());
    if (out.empty()) add(pulse::l10n::Get(pulse::l10n::StringId::PermSpecial).c_str());
    return out;
}

HANDLE OpenDetailsMetadataFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    return file;
}

DetailsMeta FetchDetailsMeta(const std::wstring& path) {
    HANDLE file = OpenDetailsMetadataFile(path);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const auto out = GetFileInformationByHandle(file, &info)
        ? FetchDetailsMeta(file, path, (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) : DetailsMeta{};
    CloseHandle(file);
    return out;
}

DetailsMeta FetchDetailsMeta(HANDLE file, const std::wstring& path, bool is_dir) {
    DetailsMeta out;
    const std::wstring shell_path = ShellPath(path);
    FetchOwner(file, out);
    FetchPermissions(path, is_dir, out);
    FetchVolume(shell_path, out);
    return out;
}

uint64_t NextDetailsWindowEpoch() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace pulse::app
