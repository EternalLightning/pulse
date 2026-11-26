// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::fs {

enum class DirectorySyncMode { LeftToRight, RightToLeft, NewerBothWays };
enum class SyncChange { AddFile, AddFolder, ReplaceFile, Link, TypeConflict, NameConflict, SameTimeConflict };

struct DirectorySyncItem {
    struct Descendant {
        std::wstring name;
        size_t parent = 0; // One-based index, with zero denoting this copy's root.
        uint64_t size = 0;
        uint64_t modified = 0;
        bool directory = false;
        bool operator==(const Descendant&) const = default;
    };

    std::wstring relative;
    std::wstring source;
    std::wstring destination_dir;
    SyncChange change{};
    size_t from = 0;
    uint64_t source_size = 0;
    uint64_t destination_size = 0;
    uint64_t source_time = 0;
    uint64_t destination_time = 0;
    std::vector<Descendant> descendants;
    bool operator==(const DirectorySyncItem&) const = default;
};

struct DirectorySyncPlan {
    std::array<std::wstring, 2> roots;
    std::array<uint64_t, 2> root_file_ids{};
    std::array<DWORD, 2> root_volumes{};
    std::vector<DirectorySyncItem> items;
    std::wstring error_path;
    DWORD error = ERROR_SUCCESS;
    bool overlap = false;
    bool operator==(const DirectorySyncPlan&) const = default;
};

// Empty result denotes the root itself. A disengaged result is outside it.
std::optional<std::wstring_view> DirectoryRelativePath(std::wstring_view root, std::wstring_view path);
std::wstring MapLinkedDirectory(const std::array<std::wstring, 2>& roots, size_t from, std::wstring_view path);
bool IsDirectorySyncCopy(SyncChange change);
DirectorySyncPlan CompareDirectories(const std::array<std::wstring, 2>& paths,
    DirectorySyncMode mode, const std::function<bool()>& cancelled = {});

} // namespace pulse::fs
