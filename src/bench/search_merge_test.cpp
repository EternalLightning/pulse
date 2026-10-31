// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/search_result_merge.h"
#include "../index/index_query.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <windows.h>

using namespace pulse::index;

namespace {

// Preserve the former value-based implementation as an independent output oracle.
SearchResult PreviousMerge(const Query& query, SearchResult local, SearchResult network) {
    SearchResult result;
    result.total = local.total + network.total;
    result.revision = (std::max)(local.revision, network.revision);
    result.error = local.error ? local.error : network.error;
    result.incomplete = local.incomplete || network.incomplete || result.error != ERROR_SUCCESS;
    result.scanning = local.scanning || network.scanning;
    result.hits.reserve(local.hits.size() + network.hits.size());
    for (auto& hit : local.hits) result.hits.push_back(std::move(hit));
    for (auto& hit : network.hits) result.hits.push_back(std::move(hit));
    const CompiledQuery compiled = ParseQuery(query.needle);
    if (query.rank || query.sort != ResultSort::Index) {
        auto compare_name = [](const Hit& a, const Hit& b) {
            const int name = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
            if (name != CSTR_EQUAL) return name == CSTR_LESS_THAN ? -1 : 1;
            const int path = CompareStringOrdinal(a.path.c_str(), -1, b.path.c_str(), -1, TRUE);
            return path == CSTR_LESS_THAN ? -1 : path == CSTR_GREATER_THAN ? 1 : 0;
        };
        std::sort(result.hits.begin(), result.hits.end(), [&](const Hit& a, const Hit& b) {
            if (query.rank) {
                const int as = RankName(a.name.data(), static_cast<uint32_t>(a.name.size()), a.is_dir, compiled);
                const int bs = RankName(b.name.data(), static_cast<uint32_t>(b.name.size()), b.is_dir, compiled);
                if (as != bs) return as > bs;
                return compare_name(a, b) < 0;
            }
            int cmp = 0;
            if (query.sort == ResultSort::Size && a.size != b.size) cmp = a.size < b.size ? -1 : 1;
            else if (query.sort == ResultSort::Mtime && a.mtime != b.mtime) cmp = a.mtime < b.mtime ? -1 : 1;
            else cmp = compare_name(a, b);
            if (query.sort_desc) cmp = -cmp;
            return cmp < 0;
        });
    }
    const size_t begin = (std::min)(query.offset, result.hits.size());
    const size_t end = (std::min)(result.hits.size(), begin + query.limit);
    if (begin) result.hits.erase(result.hits.begin(), result.hits.begin() + begin);
    if (result.hits.size() > end - begin) result.hits.resize(end - begin);
    return result;
}

bool Same(const SearchResult& a, const SearchResult& b) {
    if (a.total != b.total || a.revision != b.revision || a.incomplete != b.incomplete ||
        a.scanning != b.scanning || a.error != b.error || a.hits.size() != b.hits.size()) return false;
    for (size_t i = 0; i < a.hits.size(); ++i) {
        const Hit& x = a.hits[i];
        const Hit& y = b.hits[i];
        if (x.path != y.path || x.name != y.name || x.is_dir != y.is_dir ||
            x.size != y.size || x.mtime != y.mtime) return false;
    }
    return true;
}

SearchResult Fixture(size_t count, uint32_t seed) {
    SearchResult result;
    result.total = count + 123;
    result.revision = seed;
    result.incomplete = true;
    result.error = ERROR_ACCESS_DENIED;
    constexpr std::array<const wchar_t*, 8> names{
        L"report", L"Report", L"report.txt", L"other-report.doc", L"报告", L"文档", L"É", L""};
    for (size_t i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::wstring name = names[(seed >> 16) % names.size()];
        std::wstring path = i % 5 ? L"C:\\fixture\\" : L"\\\\server\\share\\";
        if (i % 7 == 0) path += std::wstring(300, L'x') + L"\\";
        path += name;
        // Case-insensitive equal keys retain distinct payloads, exposing tie-order changes.
        result.hits.push_back({path, name, (seed & 1u) != 0,
                               (seed >> 8) % 4u, (seed >> 12) % 3u});
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
        const auto local = Fixture(10000, 11);
        const auto network = Fixture(10000, 29);
        for (const wchar_t* needle : {L"report", L"wd"}) {
            for (bool rank : {false, true}) {
                Query query;
                query.needle = needle;
                query.rank = rank;
                query.limit = 48;
                size_t returned = 0;
                MergeSearchResults(query, local, network);
                std::array<double, 7> samples{};
                for (auto& sample : samples) {
                    const auto start = std::chrono::steady_clock::now();
                    returned += MergeSearchResults(query, local, network).hits.size();
                    sample = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start).count();
                }
                std::sort(samples.begin(), samples.end());
                std::printf("needle=%ls rank=%d candidates=20000 median_ms=%.3f returned=%zu\n",
                    needle, rank, samples[samples.size() / 2], returned);
            }
        }
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--quality-only") {
        Query query; query.limit = 10;
        SearchResult local;
        local.total = 1; local.revision = 7;
        local.hits.push_back({L"C:\\fixture\\local.txt", L"local.txt", false, 0, 0});
        SearchResult network; network.error = ERROR_TIMEOUT; network.incomplete = true;
        const auto failed = MergeSearchResults(query, local, network);
        const bool failed_ok = failed.hits.size() == 1 && failed.total == 1 && failed.revision == 7 &&
            failed.error == ERROR_TIMEOUT && failed.incomplete;
        network = {}; network.revision = 8;
        const auto recovered = MergeSearchResults(query, local, network);
        const bool recovered_ok = recovered.hits.size() == 1 && recovered.revision == 8 &&
            !recovered.error && !recovered.incomplete;
        std::printf("[%s] failed network retains local rows and quality; recovery clears quality\n",
            failed_ok && recovered_ok ? "PASS" : "FAIL");
        return failed_ok && recovered_ok ? 0 : 1;
    }
    int failures = 0;
    size_t cases = 0;
    auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    for (size_t count : {size_t{0}, size_t{1}, size_t{2}, size_t{31}, size_t{32}, size_t{33}, size_t{257}}) {
        const SearchResult local = Fixture(count / 2, 11);
        const SearchResult network = Fixture(count - count / 2, 29);
        const auto local_before = local;
        const auto network_before = network;
        const Hit* local_storage = local.hits.data();
        const Hit* network_storage = network.hits.data();
        bool equivalent = true;
        for (const wchar_t* needle : {L"", L"report", L"报告", L"wd", L"r*?", L"report | wd !other"}) {
            for (ResultSort sort : {ResultSort::Index, ResultSort::Name, ResultSort::Size, ResultSort::Mtime}) {
                for (bool rank : {false, true}) {
                    for (bool desc : {false, true}) {
                        for (size_t offset : {size_t{0}, size_t{1}, size_t{2}, size_t{31}, count, count + 1}) {
                            for (size_t limit : {size_t{0}, size_t{1}, size_t{2}, size_t{31}, size_t{32}, size_t{999}}) {
                                Query query;
                                query.needle = needle;
                                query.sort = sort;
                                query.rank = rank;
                                query.sort_desc = desc;
                                query.offset = offset;
                                query.limit = limit;
                                const auto actual = MergeSearchResults(query, local, network);
                                const auto expected = PreviousMerge(query, local, network);
                                ++cases;
                                if (!Same(actual, expected)) {
                                    std::printf("mismatch: count=%zu sort=%u rank=%d desc=%d offset=%zu limit=%zu\n",
                                        count, static_cast<unsigned>(sort), rank, desc, offset, limit);
                                    equivalent = false;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        check(equivalent, "all ranking, sort, direction and page outputs match the former implementation");
        check(Same(local, local_before) && Same(network, network_before) &&
              local.hits.data() == local_storage && network.hits.data() == network_storage,
              "provider snapshots and their storage remain unchanged");
    }
    Query query;
    query.needle = L"report";
    query.limit = 128;
    auto local = Fixture(43, 8);
    const auto network = Fixture(37, 19);
    const auto network_before = network;
    const auto previous = MergeSearchResults(query, local, network);
    local = Fixture(19, 34);
    check(Same(MergeSearchResults(query, local, network), PreviousMerge(query, local, network)) &&
          Same(network, network_before) && !previous.hits.empty(),
          "one provider update reuses the other retained subscription snapshot");
    check(Same(MergeSearchResults(query, local, local), PreviousMerge(query, local, local)),
          "aliasing provider inputs preserve duplicate hits and order");
    const auto owned = MergeSearchResults(query, Fixture(5, 42), Fixture(7, 69));
    check(Same(owned, PreviousMerge(query, Fixture(5, 42), Fixture(7, 69))),
          "returned page owns its strings after temporary inputs are destroyed");
    local.scanning = true;
    for (bool rank : {false, true}) {
        query.rank = rank;
        query.offset = 1;
        query.limit = (std::numeric_limits<size_t>::max)();
        auto bounded = query;
        bounded.limit = local.hits.size() + network.hits.size();
        check(Same(MergeSearchResults(query, local, network), PreviousMerge(bounded, local, network)),
              "maximum page limit does not overflow and retains scanning metadata");
        query.offset = (std::numeric_limits<size_t>::max)();
        bounded = query;
        bounded.limit = 0;
        check(Same(MergeSearchResults(query, local, network), PreviousMerge(bounded, local, network)),
              "out-of-range page preserves metadata without returning hits");
        query.offset = 0;
        query.limit = 0;
        check(Same(MergeSearchResults(query, local, network), PreviousMerge(query, local, network)),
              "zero-sized page preserves scanning and error metadata");
    }
    std::printf("Compared %zu query/page combinations; %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
