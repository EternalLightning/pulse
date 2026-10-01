#include "delete_operation.h"
#include "../fs/fs_recycle.h"
#include "../ipc/shell_client.h"
#include "../ipc/delete_plan_protocol.h"
#include "../common/localization.h"
#include <algorithm>
#include <chrono>
#include <set>

namespace pulse::ops {
namespace {
bool SamePath(const std::wstring& left, const std::wstring& right) {
    // Case-distinct files can coexist in a case-sensitive directory.
    return fs::NormalizePath(left) == fs::NormalizePath(right);
}

bool AddExistingRecyclePair(DeleteTarget& target, std::wstring& error) {
    if (target.physical_paths.empty()) return false;
    const auto index = fs::RecycleIndexPath(target.physical_paths.front());
    if (index.empty()) { error = L"Invalid Recycle Bin content path"; return false; }
    const DWORD attributes = GetFileAttributesW(index.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) target.physical_paths.push_back(index);
    else {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            error = L"Unable to read Recycle Bin metadata; deletion stopped.";
            return false;
        }
    }
    return true;
}
}

DeletePlan BuildDeletePlan(const OpRequest& request, uint64_t task_id, std::wstring& error) {
    DeletePlan plan;
    plan.task_id = task_id;
    plan.origin = request.is_undo ? DeleteOrigin::Undo : request.delete_origin;
    if (!IsDeleteOperation(request.type)) { error = L"Not a deletion request"; return plan; }
    std::vector<DeleteRequestTarget> requested = request.delete_targets;
    if (request.type == OpType::EmptyRecycle) {
        plan.origin = DeleteOrigin::EmptyRecycle;
        std::vector<fs::DirEntry> entries;
        const DWORD drives = GetLogicalDrives();
        bool complete = drives != 0;
        for (int i = 0; i < 26; ++i) {
            if ((drives & (1u << i)) == 0) continue;
            const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            const UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
            if (!fs::EnumerateRecycleBinAtRoot(std::wstring(root) + L"$Recycle.Bin", entries)) complete = false;
        }
        if (!complete) { error = L"Unable to enumerate the entire current-user Recycle Bin; nothing was deleted."; return plan; }
        requested.clear();
        for (const auto& entry : entries) requested.push_back({entry.full_path, {entry.recycle_path}, true});
    } else if (requested.empty()) {
        for (const auto& path : request.sources) requested.push_back({path, {path}, false});
    }
    for (const auto& item : requested) {
        DeleteTarget target;
        target.path = item.path;
        target.physical_paths = item.physical_paths;
        if (request.type == OpType::RecycleDelete) {
            // Fixed/local/NTFS/bin occupancy/candelete are not whole-batch proof.
            target.disposition = DeleteDisposition::Unknown;
            target.reason = L"Windows provides no supported read-only proof that this complete batch will be recycled.";
        } else {
            target.disposition = DeleteDisposition::Permanent;
            if (item.recycle_item && !AddExistingRecyclePair(target, error)) return {};
            target.reason = l10n::Get(item.recycle_item ? l10n::StringId::DeleteReasonRecycle
                : l10n::StringId::DeleteReasonExplicit);
        }
        plan.targets.push_back(std::move(target));
    }
    for (const auto& target : plan.targets)
        for (const auto& path : target.physical_paths)
            if (!ipc::IsLosslessDeleteShellPath(path)) {
                error = L"This path cannot be represented losslessly by the Shell deletion backend; nothing was deleted.";
                return {};
            }
    return plan;
}

bool IsDeleteSnapshotComplete(const DeletePlan& plan, const std::vector<std::wstring>& confirmed_paths) {
    if (plan.targets.empty()) return false;
    for (const auto& target : plan.targets) {
        if (target.physical_paths.empty()) return false;
        for (const auto& root : target.physical_paths)
            if (std::none_of(confirmed_paths.begin(), confirmed_paths.end(), [&](const auto& path) { return SamePath(root, path); })) return false;
    }
    return true;
}

std::vector<std::wstring> MapDeletedTargets(const DeletePlan& plan,
                                          const std::vector<std::wstring>& confirmed_paths) {
    std::vector<std::wstring> logical;
    for (const auto& target : plan.targets)
        if (!target.physical_paths.empty() && std::any_of(confirmed_paths.begin(), confirmed_paths.end(), [&](const auto& removed) {
            return SamePath(target.physical_paths.front(), removed);
        })) logical.push_back(target.path);
    return logical;
}

std::optional<DeleteConfirmation> OpsManager::PendingDeleteConfirmation() const {
    return delete_service_.Pending();
}

void OpsManager::ResolveDeleteConfirmation(uint64_t token, bool accepted) {
    std::lock_guard<std::mutex> lock(delete_wait_mutex_);
    if (delete_service_.Resolve(token, accepted)) delete_wait_cv_.notify_all();
}

std::vector<DeleteOutcome> OpsManager::DrainDeleteOutcomes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeleteOutcome> results(delete_outcomes_.begin(), delete_outcomes_.end());
    delete_outcomes_.clear();
    return results;
}

void OpsManager::FinishDelete(const QueueItem& item, std::wstring error,
                              const std::vector<std::wstring>& deleted_paths, bool executed, bool uncertain) {
    bool was_admitted = false;
    CompletedOperation completed;
    completed.type = OpType::RealDelete;
    completed.task_id = item.seq;
    completed.sources = deleted_paths;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!deleted_paths.empty()) completions_.push_back(std::move(completed));
        delete_outcomes_.push_back({item.seq, !deleted_paths.empty(), uncertain});
        if (item.recovery_sequence != 0) {
            scheduled_recovery_.erase(item.recovery_sequence);
            if (executed) std::erase_if(pending_recovery_, [&](const RecoveryEntry& entry) {
                return entry.sequence == item.recovery_sequence;
            });
        }
        if (active_item_ && active_item_->seq == item.seq) {
            was_admitted = true;
            if (uncertain) {
                RecoveryEntry recovery;
                recovery.sequence = item.seq; recovery.request = active_item_->req;
                recovery.was_active = true;
                pending_recovery_.push_back(std::move(recovery));
            }
            active_item_.reset();
        }
    }
    if (was_admitted) PersistJournal();
    delete_active_.store(false);
    delete_service_.Cancel();
    SetStatus([&](OpStatus& status) {
        status.active = false; status.percent = -1;
        status.task_id = item.seq; status.type = item.req.type;
        ++status.completed_ops;
        if (!executed) ++status.deletes_without_mutation;
        status.completed_items = deleted_paths.size();
        status.phase = !uncertain && error.empty() && !deleted_paths.empty() ? OpPhase::Completed : OpPhase::Failed;
        status.last_error = std::move(error);
        status.summary = status.phase == OpPhase::Completed
            ? l10n::Get(l10n::StringId::DeleteFinished) : status.last_error;
    });
}

void OpsManager::RunDelete(const QueueItem& item) {
    delete_cancel_.store(false);
    shell_cancel_requested_.store(false);
    delete_active_.store(true);
    if (stopping_.load()) { FinishDelete(item, l10n::Get(l10n::StringId::DeleteCancelled)); return; }
    SetStatus([&](OpStatus& status) {
        status.active = true; status.type = item.req.type; status.task_id = item.seq;
        status.phase = OpPhase::Scanning; status.percent = -1;
        status.summary = l10n::Get(l10n::StringId::DeletePreparing);
        status.last_error.clear(); status.current_item.clear();
        status.source_label.clear(); status.destination_label.clear();
        status.total_items = status.completed_items = status.total_bytes = status.transferred_bytes = 0;
        status.bytes_per_second = status.peak_bytes_per_second = 0; status.eta_seconds = 0;
    });
    std::wstring error;
    auto plan = BuildDeletePlan(item.req, item.seq, error);
    if (!error.empty()) { FinishDelete(item, std::move(error)); return; }
    const auto prepared = delete_service_.Prepare(std::move(plan));
    if (prepared.decision == DeleteDecision::Blocked) {
        FinishDelete(item, l10n::Get(l10n::StringId::DeleteUnknownStopped)); return;
    }
    if (delete_cancel_.load() || stopping_.load()) delete_service_.Cancel();
    if (prepared.decision == DeleteDecision::AwaitingConfirmation) {
        // No presenter, shutdown, rejection and timeout all fail closed.
        if (!notify_ || !IsWindow(ui_hwnd_.load())) {
            delete_service_.Cancel();
        } else {
            SetStatus([&](OpStatus& status) {
                status.phase = OpPhase::WaitingForDeleteConfirmation;
                status.summary = l10n::Get(l10n::StringId::DeleteWaiting);
            });
            std::unique_lock<std::mutex> lock(delete_wait_mutex_);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            while (delete_service_.Pending() && !delete_cancel_.load() && !stopping_.load()) {
                if (delete_wait_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    delete_service_.Cancel(); break;
                }
            }
        }
    }
    auto accepted = delete_service_.TakeAccepted(prepared.token);
    if (!accepted || delete_cancel_.load() || stopping_.load()) {
        FinishDelete(item, l10n::Get(l10n::StringId::DeleteCancelled)); return;
    }
    std::vector<std::wstring> physical;
    for (const auto& target : accepted->targets) {
        if (target.disposition != DeleteDisposition::Permanent) {
            // There is deliberately no production positive recycle backend.
            FinishDelete(item, l10n::Get(l10n::StringId::DeleteUnknownStopped)); return;
        }
        physical.insert(physical.end(), target.physical_paths.begin(), target.physical_paths.end());
    }
    QueueItem admitted = item;
    admitted.req.sources = physical;
    admitted.req.type = OpType::RealDelete; // Recovery must not expand an already confirmed empty-bin snapshot.
    admitted.req.delete_targets.clear();
    for (const auto& target : accepted->targets)
        admitted.req.delete_targets.push_back({target.path, target.physical_paths, false});
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_item_ = admitted;
        // Preserve a Recovery reservation until the backend is actually attempted.
    }
    PersistJournal();
    if (delete_cancel_.load() || stopping_.load()) {
        FinishDelete(item, l10n::Get(l10n::StringId::DeleteCancelled)); return;
    }
    SetStatus([&](OpStatus& status) {
        status.phase = OpPhase::Running; status.total_items = accepted->targets.size();
        status.summary = l10n::Get(l10n::StringId::PermanentDelete);
    });
    shell_activity_tick_ = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(done_mutex_);
        done_ready_ = false; done_deleted_paths_.clear();
    }
    auto& client = ipc::ShellClient::Instance();
    uint32_t id = 0;
    {
        // Linearize Cancel/Stop against the destructive send handoff. No mutex
        // is held during confirmation, and no backend retry is allowed.
        std::unique_lock<std::mutex> lock(delete_wait_mutex_);
        if (delete_cancel_.load() || stopping_.load()) {
            lock.unlock();
            FinishDelete(item, l10n::Get(l10n::StringId::DeleteCancelled)); return;
        }
        id = client.DeleteAuthorized(physical, prepared.token);
        current_req_id_.store(id);
    }
    if (delete_cancel_.load() || shell_cancel_requested_.load()) client.Cancel(id);
    if (id == 0) { FinishDelete(item, l10n::Get(l10n::StringId::OperationFailedMessage)); return; }
    uint32_t hr = 0; bool cancelled = false;
    if (!WaitShellDone(id, hr, cancelled, error)) {
        // WaitShellDone already bumps the finished-task counter. Preserve an
        // uncertain active plan: the backend may have mutated before Stop.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            delete_outcomes_.push_back({item.seq, false, true});
            if (active_item_ && active_item_->seq == item.seq) {
                if (item.recovery_sequence != 0) {
                    std::erase_if(pending_recovery_, [&](const RecoveryEntry& entry) { return entry.sequence == item.recovery_sequence; });
                    scheduled_recovery_.erase(item.recovery_sequence);
                }
                pending_recovery_.push_back({item.seq, active_item_->req, true});
                active_item_.reset();
            }
        }
        PersistJournal();
        delete_active_.store(false);
        return;
    }
    std::vector<std::wstring> actual;
    {
        std::lock_guard<std::mutex> lock(done_mutex_);
        for (const auto& path : done_deleted_paths_)
            if (std::any_of(physical.begin(), physical.end(), [&](const auto& requested) { return SamePath(path, requested); })) actual.push_back(path);
        done_deleted_paths_.clear();
    }
    const auto logical = MapDeletedTargets(*accepted, actual);
    const bool physical_complete = IsDeleteSnapshotComplete(*accepted, actual);
    if (cancelled) error = l10n::Get(l10n::StringId::Cancel);
    else if (FAILED(static_cast<HRESULT>(hr)) && error.empty()) error = l10n::Get(l10n::StringId::OperationFailedMessage);
    else if (!physical_complete && error.empty()) error = L"Deletion was not confirmed for every authorized physical root.";
    const bool uncertain = hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)) ||
        hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED)) ||
        hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)) ||
        error == L"Malformed deletion result" ||
        !physical_complete;
    FinishDelete(item, std::move(error), logical, true, uncertain);
}
}
