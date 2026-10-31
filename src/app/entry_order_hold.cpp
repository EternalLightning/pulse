// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "entry_order_hold.h"
#include <algorithm>
#include <unordered_map>

namespace pulse::app {
namespace {

using EntryLookup = std::unordered_map<std::wstring, size_t>;
using RenameLookup = std::unordered_map<std::wstring, std::wstring>;

EntryLookup IndexNames(const std::vector<fs::DirEntry>& entries) {
    EntryLookup lookup;
    lookup.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
        lookup.emplace(entries[index].name, index);
    return lookup;
}

RenameLookup IndexRenames(const std::vector<EntryRenameHint>& renames) {
    RenameLookup lookup;
    lookup.reserve(renames.size());
    for (const auto& rename : renames)
        lookup[rename.old_name] = rename.new_name;
    return lookup;
}

size_t FindRenamedEntry(const std::wstring& name, const EntryLookup& entries,
                       const RenameLookup& renames) {
    std::wstring key = name;
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
        latest[renames[index].old_name] = index;
    std::vector<bool> pending(renames.size(), false);
    for (const auto& entry : fresh) {
        std::wstring key = entry.name;
        for (;;) {
            const auto next = latest.find(key);
            if (next == latest.end() || pending[next->second]) break;
            const auto& rename = renames[next->second];
            const auto destination = rename.new_name;
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
