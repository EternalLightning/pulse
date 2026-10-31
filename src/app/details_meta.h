// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// details_meta.h — Security/volume facts for the details panel's 安全/其他
// sections. FetchDetailsMeta blocks (security + volume APIs); callers must
// stay off the UI thread (see PrefetchDetailsMeta in app_main.cpp).
#pragma once
#include <string>
#include <cstdint>
#include <windows.h>

namespace pulse::app {

struct DetailsMeta {
    std::wstring owner;        // 所有者 ("DOMAIN\\user"); empty = failed
    std::wstring permissions;  // rights actually granted to a file handle for the current token
    std::wstring drive;        // 驱动器 "Data (D:)"
    std::wstring file_system;  // 文件系统 "NTFS"
    std::wstring free_space;   // 可用空间 "256 GB / 500 GB (51%)"
};

DetailsMeta FetchDetailsMeta(const std::wstring& path);
DetailsMeta FetchDetailsMeta(HANDLE file, const std::wstring& path, bool is_dir);
HANDLE OpenDetailsMetadataFile(const std::wstring& path); // caller closes the returned handle
uint64_t NextDetailsWindowEpoch();

// Pure: access mask -> Chinese summary ("完全控制", "读取和执行、读取", ...).
std::wstring FormatAccessMask(unsigned int mask, bool is_dir);

} // namespace pulse::app
