#include "search_result_merge.h"
#include "index_query.h"
#include <algorithm>
#include <windows.h>

namespace pulse::index {
namespace {

int CompareName(const Hit& a, const Hit& b) {
    const int by_name = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
    if (by_name != CSTR_EQUAL) return by_name == CSTR_LESS_THAN ? -1 : 1;
    const int by_path = CompareStringOrdinal(a.path.c_str(), -1, b.path.c_str(), -1, TRUE);
    return by_path == CSTR_LESS_THAN ? -1 : by_path == CSTR_GREATER_THAN ? 1 : 0;
}

bool BetterHit(const Hit& a, const Hit& b, const Query& query) {
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
    result.revision = (std::max)(local.revision, network.revision);
    result.error = local.error ? local.error : network.error;
    result.incomplete = local.incomplete || network.incomplete || result.error != ERROR_SUCCESS;
    result.scanning = local.scanning || network.scanning;
    const size_t count = local.hits.size() + network.hits.size();
    const size_t begin = (std::min)(query.offset, count);
    const size_t end = begin + (std::min)(query.limit, count - begin);
    if (begin == end) return result;
    result.hits.reserve(end - begin);
    if (!query.rank && query.sort == ResultSort::Index) {
        for (size_t i = begin; i < end; ++i) {
            result.hits.push_back(i < local.hits.size() ? local.hits[i]
                : network.hits[i - local.hits.size()]);
        }
        return result;
    }
    if (query.rank) {
        struct RankedHit {
            const Hit* hit;
            int score;
        };
        const CompiledQuery compiled = ParseQuery(query.needle);
        std::vector<RankedHit> ranked;
        ranked.reserve(count);
        auto append = [&](const SearchResult& source) {
            for (const auto& hit : source.hits) {
                ranked.push_back({&hit, RankName(hit.name.data(),
                    static_cast<uint32_t>(hit.name.size()), hit.is_dir, compiled)});
            }
        };
        append(local);
        append(network);
        // Keep the full sort and comparator ties identical to the unscored path.
        std::sort(ranked.begin(), ranked.end(), [](const RankedHit& a, const RankedHit& b) {
            if (a.score != b.score) return a.score > b.score;
            return CompareName(*a.hit, *b.hit) < 0;
        });
        for (size_t i = begin; i < end; ++i) result.hits.push_back(*ranked[i].hit);
        return result;
    }
    std::vector<const Hit*> candidates;
    candidates.reserve(count);
    for (const auto& hit : local.hits) candidates.push_back(&hit);
    for (const auto& hit : network.hits) candidates.push_back(&hit);
    std::sort(candidates.begin(), candidates.end(), [&](const Hit* a, const Hit* b) {
        return BetterHit(*a, *b, query);
    });
    for (size_t i = begin; i < end; ++i) result.hits.push_back(*candidates[i]);
    return result;
}

} // namespace pulse::index
