#include "../app/details_meta.h"
#include "../common/localization.h"
#include <aclapi.h>
#include <cstdio>
#include <filesystem>

int main() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    using I = l10n::StringId;
    check(!l10n::Get(I::PermRead).empty() && !l10n::Get(I::PermModify).empty(),
          "load real localized permissions labels");
    const auto rw = app::FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_WRITE, false);
    check(rw.find(l10n::Get(I::PermModify)) == std::wstring::npos &&
          rw.find(l10n::Get(I::PermRead)) != std::wstring::npos &&
          rw.find(l10n::Get(I::PermWrite)) != std::wstring::npos,
          "read and write without delete or execute are not labelled modify");
    check(app::FormatAccessMask(FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE, false)
              .find(l10n::Get(I::PermModify)) != std::wstring::npos,
          "modify requires read write execute and delete");
    check(app::FormatAccessMask(FILE_ALL_ACCESS, false) == l10n::Get(I::PermFull),
          "full granted file access retains full-control label");
    const auto fixture = std::filesystem::current_path() / L"bench_data" /
        (L"details-meta-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const bool directory_created = CreateDirectoryW(fixture.c_str(), nullptr) != FALSE;
    const auto path = fixture / L"group-read.txt";
    HANDLE file = directory_created ? CreateFileW(path.c_str(), GENERIC_WRITE | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;
    check(file != INVALID_HANDLE_VALUE, "create isolated metadata security fixture");
    if (file != INVALID_HANDLE_VALUE) {
        PACL original_acl = nullptr;
        PSECURITY_DESCRIPTOR original_security = nullptr;
        const bool saved = GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original_acl, nullptr, &original_security) == ERROR_SUCCESS;
        BYTE world[SECURITY_MAX_SID_SIZE]{};
        DWORD bytes = sizeof(world);
        PACL acl = nullptr;
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_GENERIC_READ;
        entry.grfAccessMode = SET_ACCESS;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
        const bool secured = saved && CreateWellKnownSid(WinWorldSid, nullptr, world, &bytes) &&
            SetEntriesInAclW(1, &entry, nullptr, &acl) == ERROR_SUCCESS &&
            SetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
        if (acl) LocalFree(acl);
        check(secured, "grant read only through a group in the isolated fixture");
        if (secured) {
            const auto meta = app::FetchDetailsMeta(path.wstring());
            check(!meta.owner.empty() && meta.permissions.find(l10n::Get(I::PermRead)) != std::wstring::npos &&
                  meta.permissions != l10n::Get(I::PermFull) &&
                  meta.permissions.find(l10n::Get(I::PermModify)) == std::wstring::npos,
                  "kernel-granted rights include current token groups without inventing full control or modify");
        }
        check(saved && SetSecurityInfo(file, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, original_acl, nullptr) == ERROR_SUCCESS,
            "restore original isolated fixture permissions before cleanup");
        if (original_security) LocalFree(original_security);
        CloseHandle(file);
        check(DeleteFileW(path.c_str()) != FALSE, "metadata fixture file cleaned");
        const auto shared_path = fixture / L"shared-reader.txt";
        HANDLE created = CreateFileW(shared_path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
        HANDLE reader = CreateFileW(shared_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(reader != INVALID_HANDLE_VALUE, "hold a normal read-only sharing handle");
        if (reader != INVALID_HANDLE_VALUE) {
            const auto meta = app::FetchDetailsMeta(shared_path.wstring());
            check(!meta.owner.empty() && !meta.file_system.empty() && meta.permissions.empty(),
                  "sharing conflict makes only the maximum-rights summary unknown, preserving other metadata");
            CloseHandle(reader);
        }
        check(DeleteFileW(shared_path.c_str()) != FALSE, "shared reader fixture file cleaned");
    }
    if (directory_created) check(RemoveDirectoryW(fixture.c_str()) != FALSE, "metadata fixture root cleaned");
    return failures ? 1 : 0;
}
