// SPDX-License-Identifier: Apache-2.0
#include "display_alias_search.h"
#include "../fs/fs_enum.h"
#include "../index/live_filename_search.h"

namespace pulse::app {

std::vector<index::SupplementalFilenameHit> SearchDisplayAliases(
    const model::DisplayAliases& aliases, const index::Query& query,
    const std::function<bool()>& cancelled) {
    const auto compiled = index::ParseQuery(query.needle);
    if (!index::QueryHasNameFilter(compiled) || index::QueryHasContent(compiled)) return {};
    const auto prefix = model::DisplayAliasPathKey(query.path_prefix.empty()
        ? compiled.path_prefix : query.path_prefix);
    std::vector<index::SupplementalFilenameHit> hits;
    for (const auto& [path, alias] : aliases) {
        if (cancelled && cancelled()) return {};
        if (!prefix.empty() && !index::FilenameSearchCovered(path,
            prefix.back() == L'\\' ? std::wstring_view(prefix).substr(0, prefix.size() - 1) : prefix))
            continue;
        const bool alias_match = std::any_of(compiled.groups.begin(), compiled.groups.end(), [&](const auto& group) {
            return std::any_of(group.begin(), group.end(), [&](const auto& term) {
                return !term.name_not && !term.name_in_path && term.name_how != index::NameHow::Any &&
                    index::MatchName(alias.data(), static_cast<uint32_t>(alias.size()), term);
            });
        });
        if (!alias_match) continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(fs::NormalizePath(path).c_str(), GetFileExInfoStandard, &data)) continue;
        index::Hit hit;
        hit.path = path;
        hit.name = path.substr(path.find_last_of(L'\\') + 1);
        hit.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        hit.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        hit.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        if (!index::MatchLiveFilename(hit, compiled, query.folders_only, alias) ||
            index::MatchLiveFilename(hit, compiled, query.folders_only)) continue;
        const int rank = query.rank ? index::RankName(alias.data(), static_cast<uint32_t>(alias.size()),
            hit.is_dir, compiled) : 0;
        hits.push_back({std::move(hit), rank});
    }
    return hits;
}

} // namespace pulse::app
