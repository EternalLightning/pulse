#pragma once
#include "index_transport_security.h"
#include <winsvc.h>

namespace pulse::index::transport {

class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE value) : value_(value) {}
    ~ServiceHandle() { if (value_) CloseServiceHandle(value_); }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    SC_HANDLE get() const { return value_; }
private:
    SC_HANDLE value_;
};

inline bool VerifyIndexServer(HANDLE pipe, const std::wstring& helper_image, const Identity& caller) {
    if (!caller.valid) return false;
    if (VerifyServer(pipe, helper_image, caller)) return true;
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!process.get() || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) return false;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) return false;
    Handle token(raw_token);
    std::vector<BYTE> user;
    if (!TokenBytes(token.get(), TokenUser, user) ||
        !IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, WinLocalSystemSid) ||
        IsTokenRestricted(token.get())) return false;

    // SYSTEM alone is not sufficient: the endpoint must belong to the SCM's
    // running PulseIndex service and the executable registered by its admin.
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.get()) return false;
    ServiceHandle service(OpenServiceW(manager.get(), L"PulseIndex", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.get()) return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    bool valid = QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) &&
        status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId == pid &&
        status.dwServiceType == SERVICE_WIN32_OWN_PROCESS;
    std::wstring image(32768, L'\0');
    DWORD image_size = static_cast<DWORD>(image.size());
    valid = valid && QueryFullProcessImageNameW(process.get(), 0, image.data(), &image_size);
    image.resize(image_size);
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    if (!needed || needed > 65536) valid = false;
    if (valid) {
        std::vector<BYTE> config_bytes(needed);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config_bytes.data());
        valid = QueryServiceConfigW(service.get(), config, needed, &needed) &&
            config->dwServiceType == SERVICE_WIN32_OWN_PROCESS &&
            config->lpServiceStartName && config->lpBinaryPathName &&
            CompareStringOrdinal(config->lpServiceStartName, -1, L"LocalSystem", -1, TRUE) == CSTR_EQUAL;
        if (valid) {
            const auto command = L"\"" + image + L"\" --service";
            valid = CompareStringOrdinal(config->lpBinaryPathName, -1, command.c_str(), -1, TRUE) == CSTR_EQUAL;
        }
    }
    return valid;
}

} // namespace pulse::index::transport
