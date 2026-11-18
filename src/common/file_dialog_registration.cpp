// SPDX-License-Identifier: Apache-2.0
// Adapted from the Pulse upstream dialog auto-registration module.

#include "file_dialog_registration.h"
#include "current_user_security.h"
#include "file_name.h"

#include <string_view>
#include <utility>

namespace pulse::dialog_bridge {
namespace {

constexpr wchar_t kOwner[] = L"PulseDialogBridgeOwner";
constexpr wchar_t kToken[] = L"Pulse.FileDialogBridge.v1";
constexpr wchar_t kApartment[] = L"Apartment";
constexpr const wchar_t* kClasses[] = {
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}",
};

struct Key {
    HKEY handle = nullptr;
    ~Key() { if (handle) RegCloseKey(handle); }
    Key() = default;
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;
};

struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};

struct MutationLock {
    HANDLE handle = nullptr;
    DWORD error = ERROR_SUCCESS;

    MutationLock() {
        const auto sid = CurrentUserSidString();
        if (sid.empty()) { error = ERROR_ACCESS_DENIED; return; }
        const auto name = L"Global\\Pulse.FileDialogBridge.Registration.v1-" + sid;
        handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!handle) { error = GetLastError(); return; }
        const DWORD wait = WaitForSingleObject(handle, 30000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }

    ~MutationLock() {
        if (!handle) return;
        if (!error) ReleaseMutex(handle);
        CloseHandle(handle);
    }
};

DWORD Open(const wchar_t* path, Key& key, REGSAM access = KEY_READ) {
    return RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, access | RegistryView, &key.handle);
}

DWORD Create(const wchar_t* path, Key& key, DWORD* disposition = nullptr) {
    return RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0,
        KEY_READ | KEY_WRITE | RegistryView, nullptr, &key.handle, disposition);
}

DWORD ReadString(HKEY key, const wchar_t* name, std::wstring& value, bool expand = false) {
    DWORD size = 0, type = 0;
    DWORD error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (error) return error;
    const DWORD expected_type = type;
    if ((type != REG_SZ && !(expand && type == REG_EXPAND_SZ)) ||
        size < sizeof(wchar_t) || size > 65536 || size % sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::wstring text(size / sizeof(wchar_t), L'\0');
    const DWORD expected_size = size;
    error = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &size);
    if (error) return error;
    if (size != expected_size || type != expected_type || text.find(L'\0') != text.size() - 1)
        return ERROR_INVALID_DATA;
    text.pop_back();
    if (type == REG_EXPAND_SZ) {
        value.resize(32768);
        const DWORD length = ExpandEnvironmentStringsW(text.c_str(), value.data(),
            static_cast<DWORD>(value.size()));
        if (!length || length > value.size()) return ERROR_INVALID_DATA;
        value.resize(length - 1);
    } else {
        value = std::move(text);
    }
    return ERROR_SUCCESS;
}

DWORD WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

DWORD WriteEnabled(HKEY key, DWORD value) {
    return RegSetValueExW(key, L"Enabled", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

DWORD Shape(HKEY key, DWORD subkeys, DWORD values) {
    DWORD actual_subkeys = 0, actual_values = 0;
    const DWORD error = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &actual_subkeys, nullptr,
        nullptr, &actual_values, nullptr, nullptr, nullptr, nullptr);
    if (error) return error;
    return actual_subkeys == subkeys && actual_values == values ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS;
}

std::wstring_view DosPath(std::wstring_view path) {
    if (path.starts_with(LR"(\\?\)")) path.remove_prefix(4);
    if (path.size() < 4 || path.size() >= 32764 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')))
        return {};
    size_t start = 3;
    for (size_t end = 3; end <= path.size(); ++end) {
        if (end != path.size() && path[end] != L'\\') continue;
        if (!path::IsValidFileName(path.substr(start, end - start))) return {};
        start = end + 1;
    }
    return path;
}

std::wstring_view BridgePath(std::wstring_view path) {
    const auto dos = DosPath(path);
    if (dos.empty() || !path::EqualInsensitive(dos.substr(dos.find_last_of(L'\\') + 1), DllName))
        return {};
    return dos;
}

bool SamePath(std::wstring_view left, std::wstring_view right) {
    left = BridgePath(left);
    right = BridgePath(right);
    return !left.empty() && !right.empty() && path::EqualInsensitive(left, right);
}

bool ReadDllHeader(HANDLE file) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    DWORD read = 0;
    if (!ReadFile(file, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    return SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) &&
        ReadFile(file, &nt, sizeof(nt), &read, nullptr) && read == sizeof(nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0 &&
        nt.FileHeader.SizeOfOptionalHeader >= sizeof(nt.OptionalHeader) &&
        nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

DWORD OpenPackagedDll(const std::wstring& path, File& file) {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) return ERROR_INVALID_NAME;
    executable.resize(length);
    executable.resize(executable.find_last_of(L'\\') + 1);
    if (!SamePath(path, executable + DllName)) return ERROR_INVALID_NAME;
    const wchar_t drive[] = {path[0], L':', L'\\', L'\0'};
    if (GetDriveTypeW(drive) != DRIVE_FIXED) return ERROR_BAD_PATHNAME;

    const auto extended = LR"(\\?\)" + path;
    for (size_t end = 7; end <= extended.size(); ++end) {
        if (end != extended.size() && extended[end] != L'\\') continue;
        const DWORD attributes = GetFileAttributesW(extended.substr(0, end).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    }
    file.handle = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file.handle == INVALID_HANDLE_VALUE) return GetLastError();
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    const bool valid = GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &attributes,
        sizeof(attributes)) && !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ReadDllHeader(file.handle);
    return valid ? ERROR_SUCCESS : ERROR_BAD_EXE_FORMAT;
}

std::wstring ServerKey(const wchar_t* clsid) {
    return std::wstring(clsid) + L"\\InprocServer32";
}

DWORD CheckClass(const wchar_t* clsid, const std::wstring& path, bool& complete) {
    complete = false;
    Key root;
    DWORD error = Open(clsid, root);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return error;
    std::wstring owner;
    if (path.empty() || ReadString(root.handle, kOwner, owner) || owner != kToken)
        return ERROR_ALREADY_EXISTS;
    Key server;
    error = Open(ServerKey(clsid).c_str(), server);
    if (error == ERROR_FILE_NOT_FOUND) return Shape(root.handle, 0, 1);
    if (error) return error;
    if ((error = Shape(root.handle, 1, 1))) return error;
    std::wstring dll, model;
    const DWORD dll_error = ReadString(server.handle, nullptr, dll);
    const DWORD model_error = ReadString(server.handle, L"ThreadingModel", model);
    if ((dll_error != ERROR_FILE_NOT_FOUND && (dll_error || !SamePath(dll, path))) ||
        (model_error != ERROR_FILE_NOT_FOUND && (model_error || model != kApartment)))
        return ERROR_ALREADY_EXISTS;
    const DWORD values = (dll_error == ERROR_SUCCESS ? 1u : 0u) +
        (model_error == ERROR_SUCCESS ? 1u : 0u);
    error = Shape(server.handle, 0, values);
    complete = !error && values == 2;
    return error;
}

DWORD ReadJournal(HKEY key, std::wstring& path, bool& enabled) {
    if (!key) return ERROR_SUCCESS;
    DWORD error = ReadString(key, L"DllPath", path);
    if (error != ERROR_FILE_NOT_FOUND && (error || BridgePath(path).empty()))
        return error ? error : ERROR_INVALID_DATA;
    DWORD value = 0, size = sizeof(value);
    error = RegGetValueW(key, nullptr, L"Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error || size != sizeof(value) || value > 1 || (value == 1 && path.empty()))
        return error ? error : ERROR_INVALID_DATA;
    enabled = value == 1;
    return ERROR_SUCCESS;
}

DWORD RemoveClass(const wchar_t* clsid, const std::wstring& path) {
    Key root;
    DWORD error = Open(clsid, root);
    if (error == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (error) return error;
    bool complete = false;
    if ((error = CheckClass(clsid, path, complete))) return error;
    error = RegDeleteKeyExW(HKEY_CURRENT_USER, ServerKey(clsid).c_str(), RegistryView, 0);
    if (error && error != ERROR_FILE_NOT_FOUND) return error;
    if ((error = Shape(root.handle, 0, 1))) return error;
    return RegDeleteKeyExW(HKEY_CURRENT_USER, clsid, RegistryView, 0);
}

// Clear the gate first. A failed cleanup keeps the path journal for the next attempt.
DWORD ClearRegistration(HKEY config, const std::wstring& path) {
    DWORD error = WriteEnabled(config, 0);
    if (error) return error;
    for (const auto* clsid : kClasses) {
        if ((error = RemoveClass(clsid, path))) return error;
    }
    error = RegDeleteValueW(config, L"DllPath");
    if (error && error != ERROR_FILE_NOT_FOUND) return error;
    return RegFlushKey(config);
}

DWORD AddClass(const wchar_t* clsid, const std::wstring& path) {
    Key root, server;
    DWORD disposition = 0;
    DWORD error = Create(clsid, root, &disposition);
    if (error) return error;
    if (disposition != REG_CREATED_NEW_KEY) return ERROR_ALREADY_EXISTS;
    if ((error = WriteString(root.handle, kOwner, kToken))) {
        if (!Shape(root.handle, 0, 0)) RegDeleteKeyExW(HKEY_CURRENT_USER, clsid, RegistryView, 0);
        return error;
    }
    if ((error = Create(ServerKey(clsid).c_str(), server, &disposition))) return error;
    if (disposition != REG_CREATED_NEW_KEY) return ERROR_ALREADY_EXISTS;
    if ((error = WriteString(server.handle, nullptr, path))) return error;
    return WriteString(server.handle, L"ThreadingModel", kApartment);
}

} // namespace

Status Inspect() {
    Status result;
    Key config;
    DWORD error = Open(ConfigKey, config);
    if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) { result.error = error; return result; }
    bool enabled = false;
    if ((result.error = ReadJournal(config.handle, result.dll_path, enabled))) return result;
    bool complete = true;
    for (const auto* clsid : kClasses) {
        bool entry_complete = false;
        error = CheckClass(clsid, result.dll_path, entry_complete);
        if (error == ERROR_ALREADY_EXISTS) result.conflict = true;
        else if (error) result.error = error;
        complete = complete && entry_complete;
    }
    result.enabled = enabled && complete && !result.conflict && !result.error;
    return result;
}

DWORD Enable(const std::wstring& absolute_dll) {
    const std::wstring path(BridgePath(absolute_dll));
    if (path.empty()) return ERROR_INVALID_NAME;
    File file;
    DWORD error = OpenPackagedDll(path, file);
    if (error) return error;
    MutationLock lock;
    if (lock.error) return lock.error;
    const Status current = Inspect();
    if (current.error) return current.error;
    if (current.conflict || (!current.dll_path.empty() && !SamePath(path, current.dll_path)))
        return ERROR_ALREADY_EXISTS;
    if (current.enabled) return ERROR_SUCCESS;

    Key config;
    if ((error = Create(ConfigKey, config))) return error;
    if (!current.dll_path.empty() && (error = ClearRegistration(config.handle, current.dll_path))) return error;
    // Commit recovery information before exposing either COM entry.
    if ((error = WriteEnabled(config.handle, 0)) || (error = WriteString(config.handle, L"DllPath", path)) ||
        (error = RegFlushKey(config.handle))) return error;
    for (const auto* clsid : kClasses) {
        error = AddClass(clsid, path);
        if (error) break;
    }
    if (!error) error = WriteEnabled(config.handle, 1);
    if (error) ClearRegistration(config.handle, path);
    return error;
}

DWORD DisableFor(const std::wstring& absolute_dll) {
    const std::wstring path(BridgePath(absolute_dll));
    if (path.empty()) return ERROR_INVALID_NAME;
    MutationLock lock;
    if (lock.error) return lock.error;
    const Status current = Inspect();
    if (current.error) return current.error;
    if (!SamePath(path, current.dll_path)) {
        // With a missing or different journal, a live reference prevents uninstall.
        for (const auto* clsid : kClasses) {
            Key server;
            DWORD error = Open(ServerKey(clsid).c_str(), server);
            if (error == ERROR_FILE_NOT_FOUND) continue;
            if (error) return error;
            std::wstring reference;
            error = ReadString(server.handle, nullptr, reference, true);
            if (error == ERROR_FILE_NOT_FOUND) continue;
            if (error) return error;
            if (SamePath(path, reference)) return ERROR_ALREADY_EXISTS;
        }
        return ERROR_SUCCESS;
    }
    if (current.conflict) return ERROR_ALREADY_EXISTS;
    Key config;
    const DWORD error = Open(ConfigKey, config, KEY_READ | KEY_WRITE);
    return error ? error : ClearRegistration(config.handle, current.dll_path);
}

bool IsEnabledFor(const std::wstring& absolute_dll) noexcept {
    try {
        const Status current = Inspect();
        return current.enabled && SamePath(current.dll_path, absolute_dll);
    } catch (...) {
        return false;
    }
}

} // namespace pulse::dialog_bridge
