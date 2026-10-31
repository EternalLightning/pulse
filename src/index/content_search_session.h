// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "content_result_store.h"
#include <optional>

namespace pulse::index {
struct ContentSearchUpdate {
    ContentSearchProgress progress;
    std::vector<ContentHit> hits;
    std::shared_ptr<ContentResultStore> results;
};

// One request owns its accumulator until completion. Transport failures use
// the same completion path as agent replies, preserving already published rows.
class ContentSearchSession {
public:
    ContentSearchSession(const ContentSearchRequest& request, HWND notify, UINT message);
    std::optional<ContentSearchUpdate> Accept(ContentSearchUpdate update, HANDLE cancel = nullptr);
    std::optional<ContentSearchUpdate> Fail(DWORD error);
    bool Done() const { return done_; }
    std::shared_ptr<ContentResultStore> Results() const { return results_; }
private:
    uint64_t generation_;
    bool done_ = false;
    bool task_scan_ = false;
    ContentResultSort sort_ = ContentResultSort::Index;
    bool descending_ = false;
    std::vector<ContentHit> changes_;
    std::shared_ptr<ContentResultStore> results_;
    std::shared_ptr<ContentResultStore> previous_;
};
} // namespace pulse::index
