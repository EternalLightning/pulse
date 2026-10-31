// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include "../fs/drive_kind.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops {

enum class TransferStorage { Solid, Spinning, Remote, Removable, Unknown };

inline std::wstring TransferVolumeRoot(const std::wstring& path) {
    std::vector<wchar_t> root(32768);
    return GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))
        ? std::wstring(root.data()) : std::wstring{};
}

inline TransferStorage ClassifyTransferStorage(const std::wstring& path) {
    const auto root = TransferVolumeRoot(path);
    if (root.empty()) return TransferStorage::Unknown;
    if (fs::IsRemoteLikeDrive(root)) return TransferStorage::Remote;
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_REMOTE) return TransferStorage::Remote;
    if (type == DRIVE_REMOVABLE || type == DRIVE_CDROM) return TransferStorage::Removable;
    if (type != DRIVE_FIXED) return TransferStorage::Unknown;
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) return TransferStorage::Unknown;
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    const HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return TransferStorage::Unknown;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &seek, sizeof(seek), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < sizeof(seek)) return TransferStorage::Unknown;
    return seek.IncursSeekPenalty ? TransferStorage::Spinning : TransferStorage::Solid;
}

inline unsigned TransferStorageThreads(TransferStorage storage) {
    switch (storage) {
    case TransferStorage::Solid: return (std::min)(8u, (std::max)(2u, std::thread::hardware_concurrency()));
    case TransferStorage::Remote: return 4;
    case TransferStorage::Removable: return 1;
    default: return 2;
    }
}

struct TransferStoragePolicy {
    unsigned threads = 1;
    bool slow_target = false;
    bool block_clone = false;

    bool Unbuffered(uint64_t bytes) const {
        if (bytes < 64ull * 1024 * 1024 || block_clone) return false;
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        return slow_target || (GlobalMemoryStatusEx(&memory) && bytes > memory.ullAvailPhys / 2);
    }
};

inline TransferStoragePolicy TransferPolicy(const std::wstring& source, const std::wstring& destination) {
    const auto from = ClassifyTransferStorage(source), to = ClassifyTransferStorage(destination);
    TransferStoragePolicy policy;
    policy.threads = (std::min)(TransferStorageThreads(from), TransferStorageThreads(to));
    policy.slow_target = to == TransferStorage::Remote || to == TransferStorage::Removable ||
        to == TransferStorage::Spinning;
    const auto source_root = TransferVolumeRoot(source), target_root = TransferVolumeRoot(destination);
    DWORD flags = 0;
    if (!source_root.empty() && _wcsicmp(source_root.c_str(), target_root.c_str()) == 0 &&
        GetVolumeInformationW(target_root.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        policy.block_clone = (flags & 0x08000000) != 0; // FILE_SUPPORTS_BLOCK_REFCOUNTING
    return policy;
}

} // namespace pulse::ops
