#include "entry_order_hold.h"
#include "entry_sort.h"
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <unordered_map>

namespace pulse::app {
namespace {

std::wstring NameKey(std::wstring name) {
    for (auto& character : name) character = static_cast<wchar_t>(std::towlower(character));
    return name;
}

using EntryLookup = std::unordered_map<std::wstring, size_t>;
using RenameLookup = std::unordered_map<std::wstring, std::wstring>;

EntryLookup IndexNames(const std::vector<fs::DirEntry>& entries) {
    EntryLookup lookup;
    lookup.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
        lookup.emplace(NameKey(entries[index].name), index);
    return lookup;
}

RenameLookup IndexRenames(const std::vector<EntryRenameHint>& renames) {
    RenameLookup lookup;
    lookup.reserve(renames.size());
    for (const auto& rename : renames)
        lookup[NameKey(rename.old_name)] = NameKey(rename.new_name);
    return lookup;
}

size_t FindRenamedEntry(const std::wstring& name, const EntryLookup& entries,
                       const RenameLookup& renames) {
    std::wstring key = NameKey(name);
    // A surviving old name means the operation has not landed (or failed).
    for (size_t step = 0; step <= renames.size(); ++step) {
        if (const auto entry = entries.find(key); entry != entries.end()) return entry->second;
        const auto rename = renames.find(key);
        if (rename == renames.end()) break;
        key = rename->second;
    }
    return SIZE_MAX;
}

} // namespace

std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                       const std::vector<fs::DirEntry>& fresh,
                                       const std::vector<EntryRenameHint>& renames,
                                       ui::SortColumn column, ui::SortDirection direction,
                                       const std::wstring& parent) {
    if (parent.empty() || fs::IsVirtualPath(parent)) return fresh;
    const auto entries = IndexNames(fresh);
    const auto renamed = IndexRenames(renames);
    std::vector<bool> used(fresh.size(), false);
    std::vector<fs::DirEntry> result;
    result.reserve(fresh.size());
    for (const auto& previous : shown) {
        const size_t index = FindRenamedEntry(previous.name, entries, renamed);
        if (index == SIZE_MAX || used[index]) continue;
        result.push_back(fresh[index]);
        used[index] = true;
    }
    std::vector<fs::DirEntry> added;
    added.reserve(fresh.size() - result.size());
    for (size_t index = 0; index < fresh.size(); ++index)
        if (!used[index]) added.push_back(fresh[index]);
    std::stable_sort(added.begin(), added.end(), [&](const fs::DirEntry& left,
                                                  const fs::DirEntry& right) {
        return EntryLess(left, right, column, direction, parent);
    });
    result.insert(result.end(), std::make_move_iterator(added.begin()),
                  std::make_move_iterator(added.end()));
    return result;
}

void FollowEntryRenames(const std::vector<EntryRenameHint>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& selected_names, std::wstring& focus_name) {
    const auto entries = IndexNames(fresh);
    const auto renamed = IndexRenames(renames);
    const auto follow = [&](std::wstring& name) {
        if (name.empty()) return;
        const size_t index = FindRenamedEntry(name, entries, renamed);
        if (index != SIZE_MAX) name = fresh[index].name;
    };
    for (auto& name : selected_names) follow(name);
    follow(focus_name);
}

void PruneEntryRenames(std::vector<EntryRenameHint>& renames,
                      const std::vector<fs::DirEntry>& fresh) {
    std::unordered_map<std::wstring, size_t> latest;
    for (size_t index = 0; index < renames.size(); ++index)
        latest[NameKey(renames[index].old_name)] = index;
    std::vector<bool> pending(renames.size(), false);
    for (const auto& entry : fresh) {
        std::wstring key = NameKey(entry.name);
        for (;;) {
            const auto next = latest.find(key);
            if (next == latest.end() || pending[next->second]) break;
            const auto& rename = renames[next->second];
            const auto destination = NameKey(rename.new_name);
            if (key == destination) break;
            pending[next->second] = true;
            key = destination;
        }
    }
    size_t index = 0;
    std::erase_if(renames, [&](const EntryRenameHint&) {
        return !pending[index++];
    });
}

} // namespace pulse::app
