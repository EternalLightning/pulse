#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <string>
#include <vector>

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool Probe(SECURITY_ATTRIBUTES* security, const wchar_t* mode) {
    const auto name = L"\\\\.\\pipe\\PulseAuditPipeProbe." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64()) + L"." + mode;
    Handle server;
    server.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, security);
    if (server.value == INVALID_HANDLE_VALUE) {
        std::printf("mode=%ls stage=CreateNamedPipeW win32=%lu\n", mode, GetLastError()); return false;
    }
    Handle event;
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event.value) { std::printf("mode=%ls stage=CreateEventW win32=%lu\n", mode, GetLastError()); return false; }
    OVERLAPPED connected{};
    connected.hEvent = event.value;
    const BOOL immediate = ConnectNamedPipe(server.value, &connected);
    const DWORD status = immediate ? ERROR_SUCCESS : GetLastError();
    std::printf("mode=%ls stage=ConnectNamedPipe immediate=%d win32=%lu\n", mode, immediate, status);
    if (!immediate && status != ERROR_IO_PENDING && status != ERROR_PIPE_CONNECTED) return false;
    Handle client;
    client.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    const DWORD client_error = client.value == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    std::printf("mode=%ls stage=CreateFileW(client) win32=%lu\n", mode, client_error);
    if (status == ERROR_IO_PENDING) {
        if (client_error) CancelIoEx(server.value, &connected);
        const DWORD waited = WaitForSingleObject(event.value, 5000);
        if (waited != WAIT_OBJECT_0) CancelIoEx(server.value, &connected);
        DWORD ignored = 0;
        const BOOL completed = GetOverlappedResult(server.value, &connected, &ignored, TRUE);
        std::printf("mode=%ls stage=GetOverlappedResult wait=%lu success=%d win32=%lu\n", mode,
            waited, completed, completed ? ERROR_SUCCESS : GetLastError());
        if (!completed) return false;
    }
    return client_error == ERROR_SUCCESS;
}

int main() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
        std::printf("stage=OpenProcessToken win32=%lu\n", GetLastError()); return 2;
    }
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<BYTE> integrity(bytes);
    if (bytes && GetTokenInformation(token.value, TokenIntegrityLevel, integrity.data(), bytes, &bytes)) {
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data());
        const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
        std::printf("pid=%lu integrity_rid=%lu restricted=%d\n", GetCurrentProcessId(),
            *GetSidSubAuthority(label->Label.Sid, count - 1), IsTokenRestricted(token.value));
    }
    bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    if (!bytes || !GetTokenInformation(token.value, TokenUser, user.data(), bytes, &bytes)) {
        std::printf("stage=GetTokenInformation(user) win32=%lu\n", GetLastError()); return 2;
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return 2;
    const auto sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        std::printf("stage=ConvertStringSecurityDescriptorToSecurityDescriptorW win32=%lu\n", GetLastError()); return 2;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const bool exact = Probe(&attributes, L"current-user-only");
    const bool defaults = Probe(nullptr, L"default-security");
    LocalFree(descriptor);
    std::printf("RESULT current-user-only=%d default-security=%d\n", exact, defaults);
    return exact ? 0 : 1;
}
