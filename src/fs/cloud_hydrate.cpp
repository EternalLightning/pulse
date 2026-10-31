// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "cloud_hydrate.h"

#include <memory>
#include <vector>

namespace pulse::fs {

HydrateResult HydrateByReading(const std::wstring& path, const std::atomic<bool>& cancel,
                               const std::function<void(uint64_t)>& progress, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    if (cancel.load(std::memory_order_relaxed)) return HydrateResult::Cancelled;
    std::wstring target = path;
    if (target.size() >= MAX_PATH - 12 && !target.starts_with(L"\\\\?\\"))
        target = target.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + target.substr(2) : L"\\\\?\\" + target;
    // No FILE_FLAG_BACKUP_SEMANTICS: a folder fails here instead of having its
    // listing fetched.
    HANDLE file = CreateFileW(target.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = GetLastError();
        return cancel.load(std::memory_order_relaxed) ? HydrateResult::Cancelled : HydrateResult::Failed;
    }
    const auto close = [](void* handle) { CloseHandle(handle); };
    const std::unique_ptr<void, decltype(close)> owner(file, close);
    std::vector<unsigned char> buffer(size_t{1} << 20);
    uint64_t total = 0;
    HydrateResult result = HydrateResult::Done;
    for (;;) {
        if (cancel.load(std::memory_order_relaxed)) { result = HydrateResult::Cancelled; break; }
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            const DWORD failure = GetLastError();
            if (cancel.load(std::memory_order_relaxed)) {
                result = HydrateResult::Cancelled;
            } else {
                result = HydrateResult::Failed;
                if (error) *error = failure;
            }
            break;
        }
        if (!got) break;
        total += got;
        if (progress) progress(total);
    }
    return result;
}

} // namespace pulse::fs
