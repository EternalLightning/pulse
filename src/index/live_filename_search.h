#pragma once

#include "index_engine.h"
#include "search_result_merge.h"
#include <algorithm>
#include <atomic>
#include <functional>

namespace pulse::index {

inline std::wstring FilenameSearchRoot(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    if (path.rfind(L"\\\\", 0) != 0 || path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0) return {};
    const auto server = path.find(L'\\', 2);
    return server == std::wstring::npos || server == 2 || server + 1 >= path.size() || path[server + 1] == L'\\' ? std::wstring{} : path;
}

inline bool FilenameSearchCovered(std::wstring_view path, std::wstring_view root) {
    return path.size() >= root.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
        (path.size() == root.size() || path[root.size()] == L'\\');
}

inline bool MatchLiveFilename(const Hit& hit, const CompiledQuery& query, bool folders_only) {
    if (folders_only && !hit.is_dir) return false;
    if (query.groups.empty()) return true;
    for (const auto& group : query.groups) {
        bool matched = true;
        for (const auto& term : group) {
            const auto accepts = [](bool value, bool negate) { return value != negate; };
            if ((!term.exts.empty() && !accepts(MatchExt(hit.name.data(), static_cast<uint32_t>(hit.name.size()), term), term.ext_not)) ||
                (term.size_how != SizeHow::Any && !accepts(MatchSize(hit.size, term), term.size_not)) ||
                (term.date_how != DateHow::Any && !accepts(hit.mtime && MatchDate(hit.mtime, term), term.date_not))) { matched = false; break; }
            const bool name = term.name_how == NameHow::Any || (term.name_in_path
                ? MatchPath(hit.path, term)
                : MatchName(hit.name.data(), static_cast<uint32_t>(hit.name.size()), term));
            if (!MatchNameAndType(name, hit.is_dir, term)) { matched = false; break; }
        }
        if (matched) return true;
    }
    return false;
}

// Runs entirely on a cancellable background thread. Each publication replaces
// the current page; retaining only its best candidates bounds result memory.
inline void ScanLiveFilenames(const Query& query, const std::wstring& root,
                              const std::atomic<bool>& cancelled,
                              const std::function<void(SearchResult)>& publish) {
    const auto compiled = ParseQuery(query.needle);
    Query page = query;
    page.offset = 0;
    const size_t limit = (std::min)(query.limit, kSearchPageCap);
    page.limit = query.offset > kSearchPageCap - limit ? kSearchPageCap : query.offset + limit;
    SearchResult result;
    result.scanning = true;
    publish(result);
    std::vector<std::wstring> pending{root};
    ULONGLONG last_publish = GetTickCount64();
    auto flush = [&] {
        result = MergeSearchResults(page, result, {});
        publish(MergeSearchResults(query, result, {}));
        last_publish = GetTickCount64();
    };
    while (!pending.empty() && !cancelled.load()) {
        auto directory = std::move(pending.back()); pending.pop_back();
        std::wstring pattern = directory;
        if (pattern.rfind(L"\\\\?\\", 0) != 0) pattern = pattern.rfind(L"\\\\", 0) == 0
            ? L"\\\\?\\UNC\\" + pattern.substr(2) : L"\\\\?\\" + pattern;
        pattern += L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
                result.incomplete = true;
                if (!result.error) result.error = error;
            }
            continue;
        }
        struct FindOwner {
            HANDLE handle;
            ~FindOwner() { FindClose(handle); }
        } owner{find};
        bool more = true;
        while (more && !cancelled.load()) {
            if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) {
                Hit hit;
                hit.name = data.cFileName; hit.path = directory + L"\\" + hit.name;
                hit.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                hit.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                hit.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                if (hit.is_dir && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(hit.path);
                if (MatchLiveFilename(hit, compiled, query.folders_only)) { ++result.total; result.hits.push_back(std::move(hit)); }
                if (result.hits.size() > page.limit + 256 || GetTickCount64() - last_publish >= 100) flush();
            }
            more = FindNextFileW(find, &data) != FALSE;
            if (!more && GetLastError() != ERROR_NO_MORE_FILES) {
                result.incomplete = true;
                if (!result.error) result.error = GetLastError();
            }
        }
    }
    if (cancelled.load()) return;
    result.scanning = false;
    flush();
}

} // namespace pulse::index
