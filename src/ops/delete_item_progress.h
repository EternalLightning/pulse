// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "delete_service.h"
#include "../common/path_utils.h"
#include <algorithm>
#include <unordered_map>

namespace pulse::ops {

// Match MapDeletedTargets: a logical row is removed when its primary physical
// root is confirmed. Recycle metadata and recursive children are not extra rows.
class DeleteItemProgress {
public:
    void Reset(const DeletePlan& plan, uint64_t prior_completed = 0) {
        pending_.clear();
        completed_ = prior_completed;
        total_ = prior_completed + plan.targets.size();
        for (const auto& target : plan.targets)
            if (!target.physical_paths.empty()) ++pending_[Key(target.physical_paths.front())];
    }

    uint64_t Complete(const std::wstring& physical_root) {
        const auto found = pending_.find(Key(physical_root));
        if (found != pending_.end()) {
            completed_ += found->second;
            pending_.erase(found);
        }
        return completed_;
    }

    uint64_t Completed() const { return completed_; }
    uint64_t Total() const { return total_; }

private:
    std::unordered_map<std::wstring, uint64_t> pending_;
    uint64_t completed_ = 0;
    uint64_t total_ = 0;

    static std::wstring Key(const std::wstring& value) {
        auto key = path::StripExtendedPathPrefix(value);
        std::replace(key.begin(), key.end(), L'/', L'\\');
        return key; // Preserve case-distinct siblings on case-sensitive volumes.
    }
};

} // namespace pulse::ops
