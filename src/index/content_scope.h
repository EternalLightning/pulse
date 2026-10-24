#pragma once
#include "content_index.h"
#include "index_config.h"
#include <algorithm>
#include <cwctype>

namespace pulse::index {
inline std::wstring ContentScopeKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), towlower);
    return path;
}
inline bool ContentPathUnder(std::wstring_view path, std::wstring_view root) {
    return path == root || (path.size() > root.size() && path.starts_with(root) &&
        (root.ends_with(L'\\') || path[root.size()] == L'\\'));
}
inline bool ContentPathExcluded(const ContentIndexConfig& config, const std::wstring& path) {
    const auto key = ContentScopeKey(path);
    for (const auto& name : config.excluded_directories) {
        const auto part = L"\\" + ContentScopeKey(name) + L"\\";
        if ((key + L"\\").find(part) != std::wstring::npos) return true;
    }
    return std::any_of(config.excluded_paths.begin(), config.excluded_paths.end(), [&](const auto& excluded) {
        return ContentPathUnder(key, ContentScopeKey(excluded));
    });
}
inline ContentIndexConfig SharedContentScope(const ContentIndexConfig& previous,
        const std::vector<VolumeInfo>& volumes, const std::vector<std::wstring>& excluded) {
    ContentIndexConfig config = previous;
    config.shared_scope = true;
    config.roots.clear();
    config.excluded_directories.clear();
    config.excluded_paths = excluded;
    for (const auto& volume : volumes) {
        if (!volume.enabled || !volume.supported || volume.kind != VolumeKind::Fixed || volume.mount_point.empty()) continue;
        ContentIndexRoot root{ContentScopeKey(volume.mount_point), config.default_encoding};
        for (const auto& old : previous.roots)
            if (ContentScopeKey(old.path) == root.path) root.encoding = old.encoding;
        config.roots.push_back(std::move(root));
    }
    std::sort(config.roots.begin(), config.roots.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    for (auto& path : config.excluded_paths) path = ContentScopeKey(std::move(path));
    std::sort(config.excluded_paths.begin(), config.excluded_paths.end());
    config.excluded_paths.erase(std::unique(config.excluded_paths.begin(), config.excluded_paths.end()), config.excluded_paths.end());
    return config;
}
} // namespace pulse::index
