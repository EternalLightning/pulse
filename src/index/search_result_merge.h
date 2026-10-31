// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_types.h"

namespace pulse::index {

SearchResult MergeSearchResults(const Query& query, const SearchResult& local,
                                const SearchResult& network);

} // namespace pulse::index
