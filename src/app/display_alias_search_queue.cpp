// SPDX-License-Identifier: Apache-2.0
#include "display_alias_search.h"
#include "app_worker.h"

namespace pulse::app {

std::shared_ptr<const DisplayAliasSearchRequest> QueueDisplayAliasSearch(
    WorkerPool& worker, model::DisplayAliasSnapshot aliases, const index::Query& query,
    uint32_t generation, const std::shared_ptr<DisplayAliasSearchMailbox>& mailbox) {
    if (!aliases || !mailbox) return {};
    auto request = std::make_shared<const DisplayAliasSearchRequest>();
    const auto lifetime = std::weak_ptr(request);
    if (!worker.EnqueueIo([aliases = std::move(aliases), mailbox, lifetime, query, generation] {
        auto hits = SearchDisplayAliases(*aliases, query, [&] { return lifetime.expired(); });
        if (!lifetime.expired()) mailbox->Publish({generation, std::move(hits)});
    })) return {};
    return request;
}

} // namespace pulse::app
