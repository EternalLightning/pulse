// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops_manager.h"
#include "delete_operation.h"
#include "../common/file_operation_error.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <filesystem>
#include "../common/path_utils.h"

namespace pulse::ops {
namespace {
bool PermissionFailure(HRESULT error) {
    const DWORD code = format::FileOperationMessageCode(error);
    return FAILED(error) && (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ||
        code == ERROR_ELEVATION_REQUIRED);
}
}

bool OpsManager::CaptureElevationFailure(QueueItem retry, HRESULT error, const std::wstring& failed_item) {
    if (elevation_worker_ || !PermissionFailure(error) || stopping_.load() ||
        shell_cancel_requested_.load() || lock_retry_cancel_.load()) return false;
    const auto generation = lock_retry_generation_.load();
    retry.retry_failure_hr = error;
    retry.retry_failed_path = failed_item;
    retry.retry_generation = generation;
    retry.close_first.clear();
    const bool creation = retry.req.type == OpType::CreateFolder || retry.req.type == OpType::CreateTextFile;
    const bool transfer = retry.req.type == OpType::Copy || retry.req.type == OpType::Move;
    if (!retry.retry_source_directories) retry.retry_source_directories = std::make_shared<DestinationGuard>();
    std::wstring refusal;
    if (!creation && retry.retry_source_references.empty()) {
        for (size_t i = 0; i < retry.req.sources.size(); ++i) {
            const auto& source = retry.req.type == OpType::RestoreRecycle && i < retry.req.recycle_paths.size()
                ? retry.req.recycle_paths[i] : retry.req.sources[i];
            if (transfer && std::find(retry.retry_completed_sources.begin(),
                retry.retry_completed_sources.end(), source) != retry.retry_completed_sources.end()) {
                retry.retry_source_references.emplace_back();
                continue;
            }
            auto reference = FileObjectReference::Open(source);
            if (!reference.Get() || (retry.retry_rename_identity && reference.identity != retry.rename_source_id))
                return false;
            if (!retry.retry_source_directories->Capture(fs::ParentPath(source), refusal)) return false;
            retry.retry_source_references.push_back(std::move(reference));
        }
    }
    if (!creation) {
        if (retry.retry_source_references.size() != retry.req.sources.size()) return false;
        for (size_t i = 0; i < retry.req.sources.size(); ++i) {
            if (!retry.retry_source_references[i].Get()) { if (transfer) continue; return false; }
            const auto& source = retry.req.type == OpType::RestoreRecycle && i < retry.req.recycle_paths.size()
                ? retry.req.recycle_paths[i] : retry.req.sources[i];
            if (!retry.retry_source_directories->Capture(fs::ParentPath(source), refusal)) return false;
        }
    }
    if (retry.req.type == OpType::RestoreRecycle) {
        for (const auto& destination : retry.req.sources)
            if (!retry.retry_source_directories->Capture(fs::ParentPath(destination), refusal)) return false;
    }
    if (creation) {
        if (retry.req.sources.size() != 1 ||
            !retry.retry_source_directories->Capture(fs::ParentPath(retry.req.sources.front()), refusal)) return false;
    }
    if (transfer) {
        for (auto& entry : retry.retry_transfer_plan) {
            if (entry.source_identity.Get() || std::any_of(retry.retry_completed_sources.begin(),
                retry.retry_completed_sources.end(), [&](const auto& done) {
                    return entry.source == done || (entry.source.size() > done.size() &&
                        entry.source.starts_with(done) && entry.source[done.size()] == L'\\');
                })) continue;
            entry.source_identity = FileObjectReference::Open(entry.source);
            BY_HANDLE_FILE_INFORMATION metadata{};
            if (!entry.source_identity.Get() || !GetFileInformationByHandle(entry.source_identity.Get(), &metadata))
                return false;
            const uint64_t bytes = (static_cast<uint64_t>(metadata.nFileSizeHigh) << 32) | metadata.nFileSizeLow;
            if (!entry.directory && (bytes != entry.bytes ||
                CompareFileTime(&metadata.ftLastWriteTime, &entry.modified) != 0)) return false;
        }
    }
    if (retry.req.sources.empty()) return false;
    std::lock_guard lock(mutex_);
    if (!running_ || !accepting_ || stopping_.load() || generation != lock_retry_generation_.load()) return false;
    elevation_retry_ = std::move(retry);
    status_.elevation_retry_available = true;
    return true;
}

void OpsManager::RefreshElevationStatusLocked() {
    status_.elevation_retry_available = !elevation_worker_ && elevation_retry_ &&
        elevation_retry_->seq == status_.task_id &&
        elevation_retry_->retry_generation == lock_retry_generation_.load() &&
        !status_.active && status_.phase == OpPhase::Failed;
    status_.elevation_source_path.clear();
    status_.elevation_destination_path.clear();
    if (!status_.elevation_retry_available) return;
    const auto& retry = *elevation_retry_;
    const auto& req = retry.req;
    if (req.sources.empty()) return;
    size_t index = 0;
    const auto failed = std::find(req.sources.begin(), req.sources.end(), retry.retry_failed_path);
    if (failed != req.sources.end()) index = static_cast<size_t>(failed - req.sources.begin());
    else {
        const auto unfinished = std::find_if(req.sources.begin(), req.sources.end(), [&](const auto& source) {
            return std::find(retry.retry_completed_sources.begin(), retry.retry_completed_sources.end(), source) ==
                retry.retry_completed_sources.end();
        });
        if (unfinished != req.sources.end()) index = static_cast<size_t>(unfinished - req.sources.begin());
    }
    if (req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile) {
        status_.elevation_destination_path = req.sources[index];
        return;
    }
    status_.elevation_source_path = req.sources[index];
    status_.elevation_destination_path = req.dest_dir;
    if (req.type == OpType::RestoreRecycle) {
        if (index < req.recycle_paths.size()) status_.elevation_source_path = req.recycle_paths[index];
        status_.elevation_destination_path = req.sources[index];
    } else if (req.type == OpType::Rename || req.type == OpType::BatchRename) {
        const auto name = index < req.new_names.size() ? req.new_names[index] : req.new_name;
        if (!pulse::path::IsDriveRootPath(req.sources[index]))
            status_.elevation_destination_path = (std::filesystem::path(req.sources[index]).parent_path() / name).wstring();
    } else if (req.type == OpType::Copy || req.type == OpType::Move) {
        const auto planned = std::find_if(retry.retry_transfer_plan.begin(), retry.retry_transfer_plan.end(), [&](const auto& entry) {
            return entry.source == retry.retry_failed_path || entry.destination == retry.retry_failed_path;
        });
        if (planned != retry.retry_transfer_plan.end()) {
            status_.elevation_source_path = planned->source;
            status_.elevation_destination_path = planned->destination;
        }
    }
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
        if (retry.req.is_undo && (retry.req.type == OpType::Move || retry.req.type == OpType::Rename ||
            retry.req.type == OpType::BatchRename)) {
            if (pending_mutation_undo_revision_ || retry.req.undo_revision != undo_revision_ ||
                undo_.empty() || !undo_.back().supported) return false;
            pending_mutation_undo_revision_ = retry.req.undo_revision;
        }
        if (retry.req.is_undo && retry.req.type == OpType::RestoreRecycle) {
            if (pending_recycle_undo_revision_ || undo_.empty() || !undo_.back().supported ||
                undo_.back().type != OpType::RecycleDelete) return false;
            const auto& history = undo_.back();
            for (size_t i = 0; i < retry.req.sources.size(); ++i) {
                const auto source = std::find(history.sources.begin(), history.sources.end(), retry.req.sources[i]);
                const auto index = static_cast<size_t>(source - history.sources.begin());
                if (index >= history.destinations.size() || index >= history.recycle_identities.size() ||
                    i >= retry.req.recycle_paths.size() || i >= retry.req.recycle_undo_identities.size() ||
                    history.destinations[index] != retry.req.recycle_paths[i] ||
                    !(history.recycle_identities[index] == retry.req.recycle_undo_identities[i])) return false;
            }
            retry.req.undo_revision = undo_revision_;
            pending_recycle_undo_revision_ = undo_revision_;
        }
        retry.retry_batch_status = status_;
        const auto previous = std::find_if(finished_tasks_.begin(), finished_tasks_.end(),
            [task_id](const auto& task) { return task.task_id == task_id; });
        if (previous != finished_tasks_.end()) {
            retry.retry_batch_status->source_label = previous->source_label;
            retry.retry_batch_status->destination_label = previous->destination_label;
        }
        retry.elevated_retry = true;
        retry.recovery_sequence = IsDeleteOperation(retry.req.type) ? task_id : 0;
        retry.lock_retry = false;
        retry.retry_generation = lock_retry_generation_.load();
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
