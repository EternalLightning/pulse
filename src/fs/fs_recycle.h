// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// fs_recycle.h — Enumerate the per-user Recycle Bin without Shell COM.
#pragma once
#include "fs_enum.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::fs {

struct RecycleBinInfo {
    uint64_t bytes = 0;
    uint64_t items = 0;
    bool valid = false;
};

struct RecycleItem {
    std::wstring name;
    std::wstring original_path;
    std::wstring content_path; // $R* payload
    std::wstring index_path;   // $I* metadata
    uint64_t size = 0;
    FILETIME deleted{};
    bool is_dir = false;
};

bool QueryRecycleBinInfo(RecycleBinInfo& out);
bool ReadRecycleIndex(const std::wstring& index_path, RecycleItem& out);
std::wstring RecycleIndexPath(const std::wstring& content_path);
// Append live items belonging to the current user under one $Recycle.Bin root.
// Missing directories are empty. Failed enumeration, unreadable/malformed
// metadata for live payloads and orphan payloads return false; valid items are
// still appended. Payload-free indexes retained by exact restore are ignored.
bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out);
// The view uses payload attributes from the directory scan. Completeness at
// the root also requires readable metadata for each payload.
bool EnumerateRecycleViewAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out,
                                const std::function<bool()>& cancelled = {});
// Returns false on cancellation. Occupancy is valid only for a complete scan.
bool EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo& info,
                         const std::function<bool()>& cancelled);

inline bool IsRecycleViewPath(const std::wstring& path) {
    return path == L"pulse:recycle";
}

} // namespace pulse::fs
