// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "search_result_merge.h"
#include "index_query.h"
#include "search_result_order.h"
#include <algorithm>
#include <windows.h>

namespace pulse::index {
namespace {

bool BetterHit(const Hit& a, const Hit& b, const Query& query) {
    return CompareSearchOrder(a.name, a.is_dir, a.size, a.mtime,
        b.name, b.is_dir, b.size, b.mtime, query.sort, query.sort_desc) < 0;
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
        // Providers cut ranked pages by score, preserving their own order on ties.
        std::stable_sort(ranked.begin(), ranked.end(), [](const RankedHit& a, const RankedHit& b) {
            if (a.score != b.score) return a.score > b.score;
            return false;
        });
        for (size_t i = begin; i < end; ++i) result.hits.push_back(*ranked[i].hit);
        return result;
    }
    std::vector<const Hit*> candidates;
    candidates.reserve(count);
    for (const auto& hit : local.hits) candidates.push_back(&hit);
    for (const auto& hit : network.hits) candidates.push_back(&hit);
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Hit* a, const Hit* b) {
        return BetterHit(*a, *b, query);
    });
    for (size_t i = begin; i < end; ++i) result.hits.push_back(*candidates[i]);
    return result;
}

} // namespace pulse::index
