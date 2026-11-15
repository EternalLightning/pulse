// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops_manager.h"
#include "../common/file_operation_error.h"
#include "../common/path_utils.h"

namespace pulse::ops {

bool OpsManager::CaptureElevationFailure(QueueItem retry, HRESULT error, const std::wstring& failed_item) {
    // Ordinary file operations already request elevation through IFileOperation.
    if (retry.req.type != OpType::Rename || retry.req.sources.size() != 1 ||
        !pulse::path::IsDriveRootPath(retry.req.sources.front())) return false;
    const DWORD code = format::FileOperationMessageCode(error);
    if (!FAILED(error) || (code != ERROR_ACCESS_DENIED && code != ERROR_PRIVILEGE_NOT_HELD &&
        code != ERROR_ELEVATION_REQUIRED) || stopping_.load() ||
        shell_cancel_requested_.load() || lock_retry_cancel_.load()) return false;
    const auto generation = lock_retry_generation_.load();
    retry.retry_failure_hr = error;
    retry.retry_failed_path = failed_item;
    retry.retry_generation = generation;
    retry.close_first.clear();
    std::lock_guard lock(mutex_);
    if (!running_ || !accepting_ || stopping_.load() || generation != lock_retry_generation_.load()) return false;
    elevation_retry_ = std::move(retry);
    status_.elevation_retry_available = true;
    return true;
}

void OpsManager::RefreshElevationStatusLocked() {
    status_.elevation_retry_available = elevation_retry_ &&
        elevation_retry_->seq == status_.task_id &&
        elevation_retry_->retry_generation == lock_retry_generation_.load() &&
        !status_.active && status_.phase == OpPhase::Failed;
    status_.elevation_source_path.clear();
    if (status_.elevation_retry_available)
        status_.elevation_source_path = elevation_retry_->req.sources.front();
}

bool OpsManager::IsElevationFailureCurrent(uint64_t task_id) const {
    std::lock_guard lock(mutex_);
    return task_id && running_ && accepting_ && elevation_retry_ && elevation_retry_->seq == task_id &&
        elevation_retry_->retry_generation == lock_retry_generation_.load() &&
        status_.task_id == task_id && !status_.active && status_.phase == OpPhase::Failed &&
        status_.elevation_retry_available && !status_.lock_retry_available && status_.lock_owners.empty();
}

bool OpsManager::RetryElevatedOperation(uint64_t task_id) {
    {
        std::lock_guard lock(mutex_);
        if (!task_id || !running_ || !accepting_ || !elevation_retry_ || elevation_retry_->seq != task_id ||
            elevation_retry_->retry_generation != lock_retry_generation_.load() ||
            status_.task_id != task_id || status_.active || status_.phase != OpPhase::Failed ||
            !status_.elevation_retry_available || status_.lock_retry_available || !status_.lock_owners.empty()) return false;
        QueueItem retry = *elevation_retry_;
        retry.elevated_retry = true;
        retry.lock_retry = false;
        elevation_retry_.reset();
        RefreshElevationStatusLocked();
        queue_.push_front(std::move(retry));
    }
    cv_.notify_one();
    return true;
}

void OpsManager::CancelElevationRetry(uint64_t task_id) {
    bool changed = false;
    {
        std::lock_guard lock(mutex_);
        if (elevation_retry_ && elevation_retry_->seq == task_id && status_.task_id == task_id && !status_.active) {
            elevation_retry_.reset();
            RefreshElevationStatusLocked();
            changed = true;
        }
    }
    if (changed && notify_) notify_();
}
} // namespace pulse::ops
