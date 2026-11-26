// SPDX-License-Identifier: Apache-2.0
#include "directory_sync.h"
#include "fs_enum.h"
#include "../common/path_utils.h"
#include "../common/unique_resource.h"
#include <algorithm>

namespace pulse::fs {
namespace {

std::wstring Join(std::wstring_view directory, std::wstring_view child) {
    if (child.empty()) return std::wstring(directory);
    std::wstring result(directory);
    if (!result.empty() && result.back() != L'\\') result += L'\\';
    result += child;
    return result;
}

void TrimTrailingSeparator(std::wstring_view& path) {
    while (!path.empty() && path.back() == L'\\') path.remove_suffix(1);
}

bool HasDotSegment(std::wstring_view path) {
    while (!path.empty()) {
        const auto separator = path.find(L'\\');
        const auto part = path.substr(0, separator);
        if (part == L"." || part == L"..") return true;
        if (separator == path.npos) break;
        path.remove_prefix(separator + 1);
    }
    return false;
}

struct CloseDirectoryHandle {
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

DWORD ResolveRoot(const std::wstring& path, std::wstring& resolved, uint64_t& file_id, DWORD& volume) {
    if (path.empty() || IsVirtualPath(path)) return ERROR_INVALID_NAME;
    const std::wstring normalized = NormalizePath(path);
    const HANDLE raw = CreateFileW(normalized.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return GetLastError();
    const UniqueResource<HANDLE, nullptr, CloseDirectoryHandle> handle(raw);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.get(), &info)) return GetLastError();
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;

    std::wstring name(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle.get(), name.data(),
        static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) return GetLastError();
    if (length >= name.size()) return ERROR_FILENAME_EXCED_RANGE;
    name.resize(length);
    resolved = std::move(name);
    file_id = static_cast<uint64_t>(info.nFileIndexHigh) << 32 | info.nFileIndexLow;
    volume = info.dwVolumeSerialNumber;
    return ERROR_SUCCESS;
}

int CompareNames(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) - CSTR_EQUAL;
}

uint64_t PackedTime(FILETIME time) {
    return static_cast<uint64_t>(time.dwHighDateTime) << 32 | time.dwLowDateTime;
}

struct PendingFolder {
    std::array<std::wstring, 2> dirs;
    std::wstring relative;
};

struct Comparison {
    DirectorySyncPlan plan;
    const std::function<bool()>& cancelled;
    DirectorySyncMode mode;

    bool Continue() {
        if (plan.error == ERROR_SUCCESS && cancelled && cancelled()) plan.error = ERROR_CANCELLED;
        return plan.error == ERROR_SUCCESS;
    }

    bool Read(const std::wstring& dir, std::vector<DirEntry>& entries) {
        if (!Continue()) return false;
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
            (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
            plan.error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
            plan.error_path = dir;
            return false;
        }
        try {
            DirectoryEnumerator cursor(dir);
            DirEntry entry;
            while (cursor.Next(entry, cancelled) == DirectoryRead::Entry)
                entries.push_back(std::move(entry));
        } catch (const EnumerationError& error) {
            plan.error = error.Code();
            plan.error_path = dir;
            return false;
        }
        std::sort(entries.begin(), entries.end(), [](const DirEntry& lhs, const DirEntry& rhs) {
            return CompareNames(lhs.name, rhs.name) < 0;
        });
        if (std::adjacent_find(entries.begin(), entries.end(), [](const DirEntry& lhs, const DirEntry& rhs) {
                return CompareNames(lhs.name, rhs.name) == 0;
            }) != entries.end()) {
            plan.error = ERROR_DUP_NAME;
            plan.error_path = dir;
            return false;
        }
        return Continue();
    }

    bool Includes(size_t from) const {
        return mode == DirectorySyncMode::NewerBothWays ||
            (from == 0 && mode == DirectorySyncMode::LeftToRight) ||
            (from == 1 && mode == DirectorySyncMode::RightToLeft);
    }

    std::optional<std::vector<DirectorySyncItem::Descendant>> ReadPlainSubtree(const std::wstring& root) {
        struct Folder {
            std::wstring path;
            size_t index;
        };
        std::vector<DirectorySyncItem::Descendant> descendants;
        std::vector<Folder> pending{{root, 0}};
        while (!pending.empty()) {
            auto folder = std::move(pending.back());
            pending.pop_back();
            std::vector<DirEntry> entries;
            if (!Read(folder.path, entries)) return std::nullopt;
            for (auto& entry : entries) {
                if (entry.is_reparse) return std::nullopt;
                if (entry.is_dir) pending.push_back({Join(folder.path, entry.name), descendants.size() + 1});
                descendants.push_back({std::move(entry.name), folder.index, entry.size, PackedTime(entry.mtime),
                    entry.is_dir});
            }
        }
        if (!Continue()) return std::nullopt;
        return descendants;
    }

    void Add(const PendingFolder& parent, const DirEntry& entry, size_t from, SyncChange change,
             const DirEntry* destination = nullptr) {
        DirectorySyncItem item;
        item.relative = Join(parent.relative, entry.name);
        item.source = Join(parent.dirs[from], entry.name);
        item.destination_dir = parent.dirs[1 - from];
        item.change = change;
        item.from = from;
        item.source_size = entry.size;
        item.source_time = PackedTime(entry.mtime);
        if (destination) {
            item.destination_size = destination->size;
            item.destination_time = PackedTime(destination->mtime);
        }
        if (change == SyncChange::AddFolder) {
            auto descendants = ReadPlainSubtree(item.source);
            if (descendants) item.descendants = std::move(*descendants);
            else item.change = SyncChange::Link;
        }
        plan.items.push_back(std::move(item));
    }

    void Scan() {
        std::vector<PendingFolder> pending{{plan.roots, {}}};
        while (!pending.empty() && Continue()) {
            const auto parent = std::move(pending.back());
            pending.pop_back();
            std::array<std::vector<DirEntry>, 2> entries;
            if (!Read(parent.dirs[0], entries[0]) || !Read(parent.dirs[1], entries[1])) break;
            size_t left = 0, right = 0;
            while ((left < entries[0].size() || right < entries[1].size()) && Continue()) {
                const int order = left == entries[0].size() ? 1 : right == entries[1].size() ? -1 :
                    CompareNames(entries[0][left].name, entries[1][right].name);
                if (order != 0) {
                    const size_t from = order < 0 ? 0 : 1;
                    const auto& entry = order < 0 ? entries[0][left++] : entries[1][right++];
                    if (Includes(from)) Add(parent, entry, from, entry.is_reparse ? SyncChange::Link :
                        entry.is_dir ? SyncChange::AddFolder : SyncChange::AddFile);
                    continue;
                }
                const auto& a = entries[0][left++];
                const auto& b = entries[1][right++];
                if (a.is_reparse || b.is_reparse) {
                    Add(parent, a, 0, SyncChange::Link, &b);
                } else if (a.is_dir != b.is_dir) {
                    Add(parent, a, 0, SyncChange::TypeConflict, &b);
                } else if (a.is_dir) {
                    pending.push_back({{Join(parent.dirs[0], a.name), Join(parent.dirs[1], b.name)},
                        Join(parent.relative, a.name)});
                } else if (a.name != b.name) {
                    Add(parent, a, 0, SyncChange::NameConflict, &b);
                } else {
                    const LONG modified = CompareFileTime(&a.mtime, &b.mtime);
                    if (modified == 0 && a.size == b.size) continue;
                    if (mode == DirectorySyncMode::NewerBothWays && modified == 0) {
                        Add(parent, a, 0, SyncChange::SameTimeConflict, &b);
                    } else {
                        const bool from_right = mode == DirectorySyncMode::RightToLeft ||
                            (mode == DirectorySyncMode::NewerBothWays && modified < 0);
                        if (from_right) Add(parent, b, 1, SyncChange::ReplaceFile, &a);
                        else Add(parent, a, 0, SyncChange::ReplaceFile, &b);
                    }
                }
            }
        }
        if (plan.error != ERROR_SUCCESS) plan.items.clear();
    }
};

} // namespace

std::optional<std::wstring_view> DirectoryRelativePath(std::wstring_view root, std::wstring_view path) {
    TrimTrailingSeparator(root);
    TrimTrailingSeparator(path);
    if (root.empty() || path.size() < root.size() || !pulse::path::EqualInsensitive(root, path.substr(0, root.size())))
        return std::nullopt;
    if (root.size() == path.size()) return std::wstring_view{};
    if (path[root.size()] != L'\\') return std::nullopt;
    const auto relative = path.substr(root.size() + 1);
    return HasDotSegment(relative) ? std::nullopt : std::optional(relative);
}

std::wstring MapLinkedDirectory(const std::array<std::wstring, 2>& roots, size_t from, std::wstring_view path) {
    const auto relative = DirectoryRelativePath(roots[from], path);
    return relative ? Join(roots[1 - from], *relative) : std::wstring{};
}

bool IsDirectorySyncCopy(SyncChange change) {
    return change == SyncChange::AddFile || change == SyncChange::AddFolder || change == SyncChange::ReplaceFile;
}

DirectorySyncPlan CompareDirectories(const std::array<std::wstring, 2>& paths, DirectorySyncMode mode,
                                     const std::function<bool()>& cancelled) {
    Comparison scan{{}, cancelled, mode};
    for (size_t i = 0; i < paths.size() && scan.Continue(); ++i) {
        scan.plan.error = ResolveRoot(paths[i], scan.plan.roots[i], scan.plan.root_file_ids[i],
            scan.plan.root_volumes[i]);
        if (scan.plan.error != ERROR_SUCCESS) scan.plan.error_path = paths[i];
    }
    if (scan.Continue()) {
        const auto& roots = scan.plan.roots;
        const auto& ids = scan.plan.root_file_ids;
        const auto& volumes = scan.plan.root_volumes;
        scan.plan.overlap = (ids[0] && ids[0] == ids[1] && volumes[0] == volumes[1]) ||
            DirectoryRelativePath(roots[0], roots[1]).has_value() ||
            DirectoryRelativePath(roots[1], roots[0]).has_value();
        if (!scan.plan.overlap) scan.Scan();
    }
    return std::move(scan.plan);
}

} // namespace pulse::fs
