#include "snapshot_patch.h"
#include "entry_sort.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <unordered_map>

namespace pulse::app {

namespace {

std::wstring ChildPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

int FindName(const std::vector<fs::DirEntry>& entries, const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (_wcsicmp(entries[static_cast<size_t>(i)].name.c_str(), name.c_str()) == 0)
            return i;
    }
    return -1;
}

void InsertSorted(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                  ui::SortColumn col, ui::SortDirection sort_dir, const std::wstring& folder) {
    auto it = std::lower_bound(entries.begin(), entries.end(), entry,
        [col, sort_dir, &folder](const fs::DirEntry& a, const fs::DirEntry& b) {
            return EntryLess(a, b, col, sort_dir, folder);
        });
    entries.insert(it, std::move(entry));
}

} // namespace

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out) {
    if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) return false;
    const std::wstring full = fs::NormalizePath(ChildPath(dir, name));
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data))
        return false;
    out = fs::DirEntry{};
    out.name = name;
    out.attrs = data.dwFileAttributes;
    out.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.cloud_recall = (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.mtime = data.ftLastWriteTime;
    return true;
}

NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir, bool keep_order) {
    if (keep_order)
        return ApplyDirNotifyBatch(entries, folder, { event }, col, sort_dir, true);
    if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos)
        return NotifyPatch::NeedFullEnum;

    if (event.action == FILE_ACTION_REMOVED) {
        const int at = FindName(entries, event.name);
        if (at >= 0) entries.erase(entries.begin() + at);
        return NotifyPatch::Applied;
    }

    if (event.action == FILE_ACTION_RENAMED_NEW_NAME) {
        const int at = FindName(entries, event.old_name.empty() ? event.name : event.old_name);
        fs::DirEntry entry;
        if (!FillDirEntry(folder, event.name, entry)) {
            if (at >= 0) entries.erase(entries.begin() + at);
            return NotifyPatch::Applied;
        }
        if (at >= 0) entries.erase(entries.begin() + at);
        const int dup = FindName(entries, event.name);
        if (dup >= 0) entries.erase(entries.begin() + dup);
        InsertSorted(entries, std::move(entry), col, sort_dir, folder);
        return NotifyPatch::Applied;
    }

    if (event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_MODIFIED) {
        fs::DirEntry entry;
        if (!FillDirEntry(folder, event.name, entry)) {
            const int at = FindName(entries, event.name);
            if (at >= 0) entries.erase(entries.begin() + at);
            return NotifyPatch::Applied;
        }
        const int at = FindName(entries, event.name);
        if (at >= 0) entries.erase(entries.begin() + at);
        InsertSorted(entries, std::move(entry), col, sort_dir, folder);
        return NotifyPatch::Applied;
    }

    return NotifyPatch::NeedFullEnum;
}

namespace {

// Case-insensitive name key, folded per character like _wcsicmp/FindName.
struct FoldedNameHash {
    size_t operator()(const std::wstring& name) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : name) {
            hash ^= static_cast<uint64_t>(std::towlower(c));
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct FoldedNameEqual {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return true;
    }
};

NotifyPatch ApplyHeldNotifies(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                             const std::vector<fs::DirNotifyEvent>& events) {
    if (folder.empty() || fs::IsVirtualPath(folder)) return NotifyPatch::NeedFullEnum;
    std::vector<std::optional<fs::DirEntry>> renamed_entries(events.size());
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos ||
            event.old_name.find_first_of(L"\\/") != std::wstring::npos)
            return NotifyPatch::NeedFullEnum;
        if (event.action != FILE_ACTION_ADDED && event.action != FILE_ACTION_REMOVED &&
            event.action != FILE_ACTION_MODIFIED && event.action != FILE_ACTION_RENAMED_NEW_NAME)
            return NotifyPatch::NeedFullEnum;
        if (event.action == FILE_ACTION_RENAMED_NEW_NAME) {
            fs::DirEntry entry;
            // Coalesced renames can have an intermediate name already gone.
            // Keep the shown slots intact so enumeration can follow the hints.
            if (!FillDirEntry(folder, event.name, entry)) return NotifyPatch::NeedFullEnum;
            renamed_entries[index] = std::move(entry);
        }
    }
    std::vector<std::optional<fs::DirEntry>> slots;
    slots.reserve(entries.size() + events.size());
    std::unordered_map<std::wstring, size_t, FoldedNameHash, FoldedNameEqual> positions;
    positions.reserve(entries.size() + events.size());
    for (auto& entry : entries) {
        positions.emplace(entry.name, slots.size());
        slots.emplace_back(std::move(entry));
    }
    const auto erase = [&](const std::wstring& name) {
        const auto found = positions.find(name);
        if (found == positions.end()) return;
        slots[found->second].reset();
        positions.erase(found);
    };
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.action == FILE_ACTION_REMOVED) {
            erase(event.name);
            continue;
        }
        const std::wstring& previous_name = event.action == FILE_ACTION_RENAMED_NEW_NAME &&
            !event.old_name.empty() ? event.old_name : event.name;
        const auto previous = positions.find(previous_name);
        const size_t slot = previous == positions.end() ? SIZE_MAX : previous->second;
        fs::DirEntry entry;
        if (renamed_entries[index]) {
            entry = std::move(*renamed_entries[index]);
        } else if (!FillDirEntry(folder, event.name, entry)) {
            erase(previous_name);
            continue;
        }
        if (slot != SIZE_MAX) {
            positions.erase(previous_name);
            const auto duplicate = positions.find(event.name);
            if (duplicate != positions.end() && duplicate->second != slot) erase(event.name);
            slots[slot] = std::move(entry);
            positions[event.name] = slot;
        } else if (const auto duplicate = positions.find(event.name);
                   duplicate != positions.end()) {
            slots[duplicate->second] = std::move(entry);
        } else {
            positions[event.name] = slots.size();
            slots.emplace_back(std::move(entry));
        }
    }
    entries.clear();
    entries.reserve(slots.size());
    for (auto& slot : slots)
        if (slot) entries.push_back(std::move(*slot));
    return NotifyPatch::Applied;
}

} // namespace

NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir, bool keep_order) {
    if (keep_order) return ApplyHeldNotifies(entries, folder, events);
    constexpr size_t kSequentialLimit = 4;
    if (events.size() <= kSequentialLimit) {
        for (const auto& event : events) {
            if (ApplyDirNotify(entries, folder, event, col, sort_dir) == NotifyPatch::NeedFullEnum)
                return NotifyPatch::NeedFullEnum;
        }
        return NotifyPatch::Applied;
    }

    // Final state of every touched name: the on-disk name to stat, or empty
    // when the last event removed it. Later events win, as they would in order.
    std::unordered_map<std::wstring, std::wstring, FoldedNameHash, FoldedNameEqual> touched;
    touched.reserve(events.size() * 2);
    for (const auto& event : events) {
        if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos)
            return NotifyPatch::NeedFullEnum;
        switch (event.action) {
        case FILE_ACTION_REMOVED:
            touched[event.name].clear();
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            if (!event.old_name.empty()) touched[event.old_name].clear();
            touched[event.name] = event.name;
            break;
        case FILE_ACTION_ADDED:
        case FILE_ACTION_MODIFIED:
            touched[event.name] = event.name;
            break;
        default:
            return NotifyPatch::NeedFullEnum;
        }
    }

    const auto less = [col, sort_dir, &folder](const fs::DirEntry& a, const fs::DirEntry& b) {
        return EntryLess(a, b, col, sort_dir, folder);
    };
    std::vector<fs::DirEntry> fresh;
    fresh.reserve(touched.size());
    for (const auto& item : touched) {
        if (item.second.empty()) continue;
        fs::DirEntry entry;
        if (FillDirEntry(folder, item.second, entry)) fresh.push_back(std::move(entry));
    }
    std::sort(fresh.begin(), fresh.end(), less);

    std::vector<fs::DirEntry> merged;
    merged.reserve(entries.size() + fresh.size());
    auto next = fresh.begin();
    for (auto& entry : entries) {
        if (touched.find(entry.name) != touched.end()) continue;
        // InsertSorted places a new entry before the first one not less than it.
        while (next != fresh.end() && less(*next, entry)) merged.push_back(std::move(*next++));
        merged.push_back(std::move(entry));
    }
    for (; next != fresh.end(); ++next) merged.push_back(std::move(*next));
    entries = std::move(merged);
    return NotifyPatch::Applied;
}

} // namespace pulse::app
