// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "content_search_session.h"
#include "search_trace.h"

namespace pulse::index {
ContentSearchSession::ContentSearchSession(const ContentSearchRequest& request, HWND notify, UINT message)
    : generation_(request.generation),
      results_(request.paged_results ? std::make_shared<ContentResultStore>(notify, message) : nullptr),
      previous_(request.task_scan && !request.resume_previous ? nullptr : request.previous_results) {
    sort_ = request.sort; descending_ = request.sort_desc; task_scan_ = request.task_scan;
}

std::optional<ContentSearchUpdate> ContentSearchSession::Accept(ContentSearchUpdate update, HANDLE cancel) {
    if ((done_ && !update.progress.delta) || !generation_ || update.progress.generation != generation_) return std::nullopt;
    if (update.progress.subscription_error) {
        // A subscription failure cannot replace the initial scan outcome or rows.
        update.hits.clear();
        update.results = results_;
        return update;
    }
    if (update.progress.delta && results_) {
        changes_.insert(changes_.end(), std::make_move_iterator(update.hits.begin()), std::make_move_iterator(update.hits.end()));
        if (!update.progress.done) return std::nullopt;
        if (!update.progress.error && !(task_scan_ ? results_->StreamUpsert(changes_,sort_,descending_) :
            results_->ApplyChanges(changes_,sort_,descending_))) update.progress.error = results_->Error();
        std::vector<ContentHit>().swap(changes_);
        update.hits.clear(); update.results = results_;
        return update;
    }
    if (results_) {
        if (!(task_scan_ ? results_->StreamUpsert(update.hits,sort_,descending_) : results_->Append(update.hits))) {
            update.progress.error = results_->Error();
            update.progress.done = true;
        } else if (task_scan_) {
            for (const auto& hit:update.hits) TraceSearch("content_task_visible",generation_,hit.path);
        }
        update.hits.clear();
        // An empty progress notification is not a replacement result list.
        if (results_->RawCount() || update.progress.done) update.results = results_;
    }
    done_ = update.progress.done;
    if (previous_) {
        if (!done_) return std::nullopt;
        if (update.progress.error || update.progress.truncated ||
            (results_ && results_->SameContents(*previous_, cancel)))
            update.results = previous_;
        if(update.progress.live && !update.progress.error) { results_ = update.results; previous_.reset(); }
    }
    return update;
}

std::optional<ContentSearchUpdate> ContentSearchSession::Fail(DWORD error) {
    ContentSearchUpdate update;
    update.progress.generation = generation_;
    update.progress.done = true;
    update.progress.error = error;
    return Accept(std::move(update));
}
} // namespace pulse::index
