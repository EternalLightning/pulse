#pragma once
#include <windows.h>
#include <winsvc.h>

namespace pulse::index {
// Return the control submission result, not rebuild completion.
template<class OpenManager, class OpenService, class Control, class Close, class LastError>
DWORD SubmitServiceReload(const wchar_t* name, DWORD code, OpenManager open_manager,
                         OpenService open_service, Control control, Close close, LastError last_error) {
    const SC_HANDLE manager = open_manager();
    if (!manager) return last_error();
    const SC_HANDLE service = open_service(manager, name);
    if (!service) {
        const DWORD error = last_error();
        close(manager);
        return error;
    }
    SERVICE_STATUS status{};
    const DWORD error = control(service, code, &status) ? ERROR_SUCCESS : last_error();
    close(service);
    close(manager);
    return error;
}
inline DWORD SubmitServiceReload(const wchar_t* name, DWORD code) {
    return SubmitServiceReload(name, code,
        [] { return OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT); },
        [](SC_HANDLE manager, const wchar_t* service) { return OpenServiceW(manager, service, SERVICE_USER_DEFINED_CONTROL); },
        [](SC_HANDLE service, DWORD control, SERVICE_STATUS* status) { return ControlService(service, control, status); },
        [](SC_HANDLE handle) { CloseServiceHandle(handle); }, [] { return GetLastError(); });
}
}
