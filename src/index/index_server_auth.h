// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_transport_security.h"
#include <winsvc.h>
#include <string_view>

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

inline bool IndexServiceOwnsEndpoint(ULONG pid, const SERVICE_STATUS_PROCESS& status,
                                     const QUERY_SERVICE_CONFIGW& config) {
    if (!pid || status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId != pid ||
        status.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        config.dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        !config.lpServiceStartName || !config.lpBinaryPathName ||
        CompareStringOrdinal(config.lpServiceStartName, -1, L"LocalSystem", -1, TRUE) != CSTR_EQUAL)
        return false;
    const std::wstring_view command(config.lpBinaryPathName);
    const auto closing_quote = command.find(L'"', 1);
    return command.starts_with(L"\"") && closing_quote != std::wstring_view::npos &&
        closing_quote > 1 && command.substr(closing_quote) == L"\" --service";
}

inline bool VerifyIndexServer(HANDLE pipe, const std::wstring& helper_image, const Identity& caller) {
    if (!caller.valid) return false;
    if (VerifyServer(pipe, helper_image, caller)) return true;
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid)) return false;

    // Ordinary users cannot open a SYSTEM service's process/token. Authenticate
    // its pipe PID against SCM instead; helper authentication above stays exact.
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager.get()) return false;
    ServiceHandle service(OpenServiceW(manager.get(), L"PulseIndex", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.get()) return false;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    if (!needed || needed > 65536) return false;
    std::vector<BYTE> config_bytes(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config_bytes.data());
    if (!QueryServiceConfigW(service.get(), config, needed, &needed)) return false;
    return QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) &&
        IndexServiceOwnsEndpoint(pid, status, *config);
}

inline HANDLE OpenVerifiedIndexPipe(const std::wstring& pipe_name,
                                    const std::wstring& helper_image = SiblingIndexImage()) {
    Handle pipe(CreateFileW(pipe_name.c_str(), kClientPipeAccess, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    if (!VerifyIndexServer(pipe.get(), helper_image, Identity::Current())) {
        CloseHandle(pipe.release());
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return pipe.release();
}

} // namespace pulse::index::transport
