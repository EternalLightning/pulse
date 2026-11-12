// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../common/file_operation_error.h"
#include "../common/localization.h"
#include "../common/unique_resource.h"
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ops {

inline bool Sha256File(const std::wstring& path, const std::atomic<bool>& cancel,
                      const std::atomic<bool>& pause, std::array<uint8_t, 32>& digest,
                      std::wstring& error) {
    if (cancel.load()) return false;
    struct CloseAlgorithm { void operator()(BCRYPT_ALG_HANDLE value) const noexcept { BCryptCloseAlgorithmProvider(value, 0); } };
    struct CloseHash { void operator()(BCRYPT_HASH_HANDLE value) const noexcept { BCryptDestroyHash(value); } };
    struct CloseFile { void operator()(HANDLE value) const noexcept { CloseHandle(value); } };
    const auto fail = [&](HRESULT code) {
        if (error.empty()) error = format::FileOperationErrorText(code) + L" | " + path;
        return false;
    };
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const NTSTATUS opened = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    UniqueResource<BCRYPT_ALG_HANDLE, nullptr, CloseAlgorithm> algorithm_owner(algorithm);
    DWORD object_bytes = 0, hash_bytes = 0, returned = 0;
    if (opened < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_bytes), sizeof(hash_bytes), &returned, 0) < 0 ||
        hash_bytes != digest.size()) {
        error = l10n::Get(l10n::StringId::OperationHashInitializeFailed);
        return false;
    }
    std::vector<uint8_t> object(object_bytes);
    BCRYPT_HASH_HANDLE hash = nullptr;
    const NTSTATUS created = BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0);
    UniqueResource<BCRYPT_HASH_HANDLE, nullptr, CloseHash> hash_owner(hash);
    if (created < 0) {
        error = l10n::Get(l10n::StringId::OperationHashInitializeFailed);
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return fail(HRESULT_FROM_WIN32(code));
    }
    UniqueResource<HANDLE, nullptr, CloseFile> file_owner(file);
    std::vector<uint8_t> buffer(1024 * 1024);
    while (!cancel.load()) {
        while (pause.load() && !cancel.load()) Sleep(20);
        if (cancel.load()) break;
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            const DWORD code = GetLastError();
            return fail(HRESULT_FROM_WIN32(code));
        }
        if (read == 0) break;
        const NTSTATUS hashed = BCryptHashData(hash, buffer.data(), read, 0);
        if (hashed < 0) return fail(HRESULT_FROM_NT(hashed));
    }
    file_owner.reset();
    if (cancel.load()) return false;
    const NTSTATUS finished = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    if (finished < 0) return fail(HRESULT_FROM_NT(finished));
    return !cancel.load();
}

} // namespace pulse::ops
