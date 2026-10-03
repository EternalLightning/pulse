#pragma once
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
namespace pulse::index::transport {
inline constexpr DWORD kClientPipeAccess = FILE_GENERIC_READ | (FILE_GENERIC_WRITE & ~FILE_APPEND_DATA);
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
inline bool TokenBytes(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE>& bytes) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (!size || size > 1024 * 1024) return false;
    bytes.resize(size);
    return GetTokenInformation(token, kind, bytes.data(), size, &size) != FALSE;
}
inline std::wstring SidString(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring value(text); LocalFree(text); return value;
}
struct Identity {
    std::wstring user, logon, integrity, grants;
    DWORD session = 0, elevation = 0, restricted = 0, app_container = 0;
    LUID authentication{};
    bool valid = false;
    static Identity FromToken(HANDLE token) {
        Identity value;
        std::vector<BYTE> bytes;
        if (!TokenBytes(token, TokenUser, bytes)) return value;
        value.user = SidString(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
        if (!TokenBytes(token, TokenGroups, bytes)) return value;
        const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
        for (DWORD i = 0; i < groups->GroupCount; ++i)
            if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
                value.logon = SidString(groups->Groups[i].Sid);
        for (const auto kind : {TokenGroups, TokenRestrictedSids, TokenPrivileges}) {
            if (!TokenBytes(token, kind, bytes)) return value;
            std::vector<std::wstring> entries;
            if (kind == TokenPrivileges) {
                const auto* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(bytes.data());
                for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
                    const auto& entry = privileges->Privileges[i];
                    entries.push_back(std::to_wstring(entry.Luid.HighPart) + L":" + std::to_wstring(entry.Luid.LowPart) + L":" + std::to_wstring(entry.Attributes));
                }
            } else {
                const auto* entries_raw = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
                for (DWORD i = 0; i < entries_raw->GroupCount; ++i)
                    entries.push_back(SidString(entries_raw->Groups[i].Sid) + L":" + std::to_wstring(entries_raw->Groups[i].Attributes));
            }
            std::sort(entries.begin(), entries.end());
            for (const auto& entry : entries) value.grants += std::to_wstring(kind) + L":" + entry + L";";
        }
        if (!TokenBytes(token, TokenIntegrityLevel, bytes)) return value;
        value.integrity = SidString(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid);
        TOKEN_STATISTICS statistics{}; DWORD size = 0;
        if (!GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &size) ||
            !GetTokenInformation(token, TokenSessionId, &value.session, sizeof(value.session), &size) ||
            !GetTokenInformation(token, TokenElevationType, &value.elevation, sizeof(value.elevation), &size) ||
            !GetTokenInformation(token, TokenHasRestrictions, &value.restricted, sizeof(value.restricted), &size) ||
            !GetTokenInformation(token, TokenIsAppContainer, &value.app_container, sizeof(value.app_container), &size)) return value;
        value.authentication = statistics.AuthenticationId;
        value.valid = !value.user.empty() && !value.logon.empty() && !value.integrity.empty();
        return value;
    }
    static Identity Current() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
        Handle owner(token); return FromToken(token);
    }
    bool Matches(const Identity& other) const {
        return valid && other.valid && user == other.user && logon == other.logon &&
            session == other.session && authentication.LowPart == other.authentication.LowPart &&
            authentication.HighPart == other.authentication.HighPart && integrity == other.integrity &&
            elevation == other.elevation && restricted == other.restricted && app_container == other.app_container && grants == other.grants;
    }
    std::wstring Suffix() const {
        if (!valid) return {};
        return user + L"." + std::to_wstring(session) + L"." +
            std::to_wstring(static_cast<DWORD>(authentication.HighPart)) + L"." + std::to_wstring(authentication.LowPart) +
            L"." + integrity + L"." + std::to_wstring(elevation) + L"." + std::to_wstring(restricted);
    }
};
class LogonSecurity {
public:
    explicit LogonSecurity(const Identity& identity) {
        if (!identity.valid) return;
        const auto sddl = L"D:P(A;;GA;;;" + identity.logon + L")";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            attributes_ = {sizeof(attributes_), descriptor_, FALSE};
    }
    ~LogonSecurity() { if (descriptor_) LocalFree(descriptor_); }
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};
inline void RevertOrFailFast() {
    // Continuing a privileged worker under a failed-to-revert caller token can
    // corrupt storage/authorization assumptions. This is not recoverable IO.
    if (!RevertToSelf()) RaiseFailFastException(nullptr, nullptr, 0);
}
// Call only after reading a message: named-pipe impersonation represents the
// token that actually sent that message, not the connecting process's primary token.
inline HANDLE CaptureCaller(HANDLE pipe) {
    if (!ImpersonateNamedPipeClient(pipe)) return nullptr;
    HANDLE source = nullptr, result = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &source)) {
        DuplicateTokenEx(source, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &result);
        CloseHandle(source);
    }
    RevertOrFailFast();
    return result;
}
inline bool VerifyServer(HANDLE pipe, const std::wstring& expected_image, const Identity& expected) {
    ULONG pid = 0;
    if (!expected.valid || !GetNamedPipeServerProcessId(pipe, &pid)) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.get() || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return false;
    std::wstring image(32768, L'\0'); DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &size)) return false;
    image.resize(size);
    if (CompareStringOrdinal(image.c_str(), -1, expected_image.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &token)) return false;
    Handle owner(token); return expected.Matches(Identity::FromToken(token));
}
inline bool Transfer(HANDLE pipe, void* value, DWORD count, bool write, ULONGLONG deadline, HANDLE cancel = nullptr) {
    auto* bytes = static_cast<BYTE*>(value);
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.get()) return false;
    while (count) {
        const auto now = GetTickCount64();
        if (now >= deadline || (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0)) {
            SetLastError(now >= deadline ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED); return false;
        }
        OVERLAPPED operation{}; operation.hEvent = event.get(); ResetEvent(event.get()); DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, bytes, count, &done, &operation) : ReadFile(pipe, bytes, count, &done, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[]{event.get(), cancel};
            const auto wait = WaitForMultipleObjects(cancel ? 2u : 1u, waits, FALSE,
                static_cast<DWORD>((std::min)(deadline - now, static_cast<ULONGLONG>(MAXDWORD - 1))));
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &done, TRUE);
                SetLastError(wait == WAIT_OBJECT_0 + 1 ? ERROR_OPERATION_ABORTED : ERROR_TIMEOUT); return false;
            }
            ok = GetOverlappedResult(pipe, &operation, &done, FALSE);
        }
        if (!ok || !done) return false;
        bytes += done; count -= done;
    }
    return true;
}
inline std::wstring SiblingIndexImage() {
    std::wstring image(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!size || size >= image.size()) return {};
    image.resize(size); const auto slash = image.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : image.substr(0, slash + 1) + L"Pulse.Index.exe";
}
} // namespace pulse::index::transport
