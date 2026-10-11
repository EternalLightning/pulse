#include "snapshot_patch.h"
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace pulse::app {

namespace {

std::wstring ChildPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
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

ResolvedDirNotifies ResolveDirNotifies(const std::wstring& folder,
                                     const std::vector<fs::DirNotifyEvent>& events) {
    ResolvedDirNotifies resolved;
    if (folder.empty() || fs::IsVirtualPath(folder)) return resolved;
    resolved.events = events;
    resolved.entries.resize(events.size());
    for (size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.name.empty() || event.name.find_first_of(L"\\/") != std::wstring::npos ||
            event.old_name.find_first_of(L"\\/") != std::wstring::npos)
            return resolved;
        if (event.action != FILE_ACTION_ADDED && event.action != FILE_ACTION_REMOVED &&
            event.action != FILE_ACTION_MODIFIED && event.action != FILE_ACTION_RENAMED_NEW_NAME)
            return resolved;
        if (event.action != FILE_ACTION_REMOVED) {
            fs::DirEntry entry;
            // A failed stat is inconclusive. Validate the whole batch before
            // moving any held row so the caller can enumerate without publishing loss.
            if (!FillDirEntry(folder, event.name, entry)) return resolved;
            resolved.entries[index] = std::move(entry);
        }
    }
    resolved.status = NotifyPatch::Applied;
    return resolved;
}

NotifyPatch ApplyResolvedDirNotifies(std::vector<fs::DirEntry>& entries,
                                    const ResolvedDirNotifies& resolved) {
    if (resolved.status != NotifyPatch::Applied ||
        resolved.entries.size() != resolved.events.size()) return NotifyPatch::NeedFullEnum;
    const auto& events = resolved.events;
    std::vector<std::optional<fs::DirEntry>> slots;
    slots.reserve(entries.size() + events.size());
    std::unordered_map<std::wstring, size_t> positions;
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
        fs::DirEntry entry = resolved.entries[index];
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

} // namespace pulse::app
