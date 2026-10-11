#pragma once
#include <windows.h>
#include <winternl.h>
#include <climits>
#include <string>

namespace pulse {
inline HANDLE CreateDirectoryHandle(const std::wstring& path, bool reparse = false) {
    using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using StatusError = ULONG (NTAPI*)(NTSTATUS);
    static const auto create = reinterpret_cast<Create>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    static const auto status_error = reinterpret_cast<StatusError>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (!create || path.empty()) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
    std::wstring native = path;
    if (native.starts_with(L"\\\\?\\")) native = L"\\??\\" + native.substr(4);
    else if (native.starts_with(L"\\\\")) native = L"\\??\\UNC\\" + native.substr(2);
    else native = L"\\??\\" + native;
    if (native.size() * sizeof(wchar_t) > USHRT_MAX) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return nullptr; }
    UNICODE_STRING name{};
    name.Buffer = native.data();
    name.Length = static_cast<USHORT>(native.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    IO_STATUS_BLOCK status{};
    HANDLE handle = nullptr;
    constexpr ULONG create_new = 2;
    constexpr ULONG directory_file = 0x00000001;
    constexpr ULONG synchronous = 0x00000020;
    constexpr ULONG open_reparse = 0x00200000;
    const ACCESS_MASK access = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE |
        (reparse ? GENERIC_WRITE | DELETE : 0);
    const NTSTATUS result = create(&handle, access, &attributes, &status, nullptr, FILE_ATTRIBUTE_DIRECTORY,
        FILE_SHARE_READ, create_new, directory_file | synchronous | open_reparse, nullptr, 0);
    if (result < 0 || !handle) {
        SetLastError(status_error ? status_error(result) : ERROR_ACCESS_DENIED);
        return nullptr;
    }
    return handle;
}
}
