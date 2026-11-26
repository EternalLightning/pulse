// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../index/search_result_merge.h"
#include "../common/window_result_mailbox.h"
#include "../model/display_aliases.h"
#include <functional>

namespace pulse::app {

class WorkerPool;
struct DisplayAliasSearchResult {
    uint32_t generation = 0;
    std::vector<index::SupplementalFilenameHit> hits;
};
using DisplayAliasSearchMailbox = WindowResultMailbox<DisplayAliasSearchResult>;
struct DisplayAliasSearchRequest {};

std::vector<index::SupplementalFilenameHit> SearchDisplayAliases(
    const model::DisplayAliases& aliases, const index::Query& query,
    const std::function<bool()>& cancelled = {});

std::shared_ptr<const DisplayAliasSearchRequest> QueueDisplayAliasSearch(
    WorkerPool& worker, model::DisplayAliasSnapshot aliases, const index::Query& query,
    uint32_t generation, const std::shared_ptr<DisplayAliasSearchMailbox>& mailbox);

} // namespace pulse::app
