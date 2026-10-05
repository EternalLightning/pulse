#include "search_result_merge.h"
#include <algorithm>

namespace pulse::index {
namespace {

int CompareName(const Hit& a, const Hit& b) {
    const int by_name = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
    if (by_name != CSTR_EQUAL) return by_name == CSTR_LESS_THAN ? -1 : 1;
    const int by_path = CompareStringOrdinal(a.path.c_str(), -1, b.path.c_str(), -1, TRUE);
    return by_path == CSTR_LESS_THAN ? -1 : by_path == CSTR_GREATER_THAN ? 1 : 0;
}

bool BetterHit(const Hit& a, const Hit& b, const Query& query,
               const CompiledQuery& compiled) {
    if (query.rank) {
        const int as = RankName(a.name.data(), static_cast<uint32_t>(a.name.size()), a.is_dir, compiled);
        const int bs = RankName(b.name.data(), static_cast<uint32_t>(b.name.size()), b.is_dir, compiled);
        if (as != bs) return as > bs;
        return CompareName(a, b) < 0;
    }
    int cmp = 0;
    if (query.sort == ResultSort::Size && a.size != b.size) cmp = a.size < b.size ? -1 : 1;
    else if (query.sort == ResultSort::Mtime && a.mtime != b.mtime) cmp = a.mtime < b.mtime ? -1 : 1;
    else cmp = CompareName(a, b);
    if (query.sort_desc) cmp = -cmp;
    return cmp < 0;
}

} // namespace

SearchResult MergeSearchResults(const Query& query, const SearchResult& local,
                                const SearchResult& network) {
    SearchResult result;
    result.total = local.total + network.total;
    std::vector<const Hit*> candidates;
    candidates.reserve(local.hits.size() + network.hits.size());
    for (const auto& hit : local.hits) candidates.push_back(&hit);
    for (const auto& hit : network.hits) candidates.push_back(&hit);
    const CompiledQuery compiled = ParseQuery(query.needle);
    if (query.rank || query.sort != ResultSort::Index) {
        std::sort(candidates.begin(), candidates.end(), [&](const Hit* a, const Hit* b) {
            return BetterHit(*a, *b, query, compiled);
        });
    }
    const size_t begin = (std::min)(query.offset, candidates.size());
    const size_t end = (std::min)(candidates.size(), begin + query.limit);
    result.hits.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) result.hits.push_back(*candidates[i]);
    return result;
}

} // namespace pulse::index
