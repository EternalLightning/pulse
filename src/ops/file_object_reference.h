#pragma once

#include "file_identity.h"
#include <memory>
#include <string>

namespace pulse::ops {

struct FileObjectReference {
    struct Lease {
        HANDLE value = nullptr;
        explicit Lease(HANDLE handle) : value(handle) {}
        ~Lease() { Close(); }
        void Close() { if (value) CloseHandle(value); value = nullptr; }
    };
    FileIdentity identity;
    std::shared_ptr<Lease> handle;
    HANDLE Get() const { return handle ? handle->value : nullptr; }

    static FileObjectReference Open(const std::wstring& path,
        decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) {
        FileObjectReference result;
        HANDLE raw = CreateFileW(path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) return result;
        result.handle = std::make_shared<Lease>(raw);
        if (!ReadFileIdentity(raw, result.identity, query)) result.handle.reset();
        return result;
    }

    bool Matches(HANDLE other,
        decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) const {
        FileIdentity original, current;
        return Get() && ReadFileIdentity(Get(), original, query) &&
            ReadFileIdentity(other, current, query) && original == identity && current == original;
    }

    // The path selects a handle; all mutation after validation uses that handle.
    // Retaining the original reference also prevents FileID reuse after replacement.
    bool Delete(const std::wstring& path, DWORD& error,
        decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) const {
        HANDLE raw = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
        const auto close = std::shared_ptr<void>(raw, [](void* value) { CloseHandle(value); });
        if (!Matches(raw, query)) { error = ERROR_FILE_INVALID; return false; }
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(raw, FileBasicInfo, &basic, sizeof(basic))) {
            error = GetLastError(); return false;
        }
        if (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) {
            basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
            if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileInformationByHandle(raw, FileBasicInfo, &basic, sizeof(basic))) {
                error = GetLastError(); return false;
            }
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(raw, FileDispositionInfo, &disposition, sizeof(disposition))) {
            error = GetLastError(); return false;
        }
        error = ERROR_SUCCESS;
        handle->Close();
        return true;
    }
};

} // namespace pulse::ops
