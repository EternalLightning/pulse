#pragma once
#include "index_engine.h"

namespace pulse::index {

SearchResult MergeSearchResults(const Query& query, const SearchResult& local,
                                const SearchResult& network);

} // namespace pulse::index
