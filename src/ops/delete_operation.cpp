#include "delete_operation.h"
#include "../fs/fs_recycle.h"
#include "../ipc/shell_client.h"
#include "../ipc/delete_plan_protocol.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <winnetwk.h>
#include <algorithm>
#include <chrono>
#include <set>

namespace pulse::ops {
namespace {
bool SamePath(const std::wstring& left, const std::wstring& right) {
    // Case-distinct files can coexist in a case-sensitive directory.
    return fs::NormalizePath(left) == fs::NormalizePath(right);
}

bool IsNetworkDeletionPath(std::wstring_view path) {
    const auto parsed = pulse::path::StripExtendedPathPrefix(path);
    if (parsed.starts_with(L"\\\\")) return true;
    if (parsed.size() < 3 || parsed[1] != L':') return false;
    const wchar_t drive[] = {parsed[0], L':', 0};
    const wchar_t root[] = {parsed[0], L':', L'\\', 0};
    if (GetDriveTypeW(root) == DRIVE_REMOTE) return true;
    wchar_t remote[32768]{};
    DWORD size = ARRAYSIZE(remote);
    return WNetGetConnectionW(drive, remote, &size) == NO_ERROR;
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
            const bool network = std::any_of(target.physical_paths.begin(), target.physical_paths.end(), IsNetworkDeletionPath);
            target.disposition = network ? DeleteDisposition::Permanent : DeleteDisposition::RecycleRequested;
            if (network) target.reason = l10n::Get(l10n::StringId::DeleteReasonNetwork);
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
                              const std::vector<std::wstring>& deleted_paths, bool executed, bool uncertain,
                               const std::vector<std::wstring>& recycled_paths,
                               const std::vector<std::wstring>& recycle_destinations, bool cancelled) {
    bool was_admitted = false;
    CompletedOperation completed;
    completed.type = OpType::RealDelete;
    completed.task_id = item.seq;
    completed.sources = deleted_paths;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!deleted_paths.empty()) completions_.push_back(std::move(completed));
        if (!recycled_paths.empty()) {
            CompletedOperation recycled;
            recycled.type = OpType::RecycleDelete; recycled.task_id = item.seq; recycled.sources = recycled_paths;
            completions_.push_back(std::move(recycled));
            if (!item.req.is_undo) {
                UndoEntry undo;
                undo.type = OpType::RecycleDelete; undo.sources = recycled_paths;
                undo.destinations = recycle_destinations;
                undo.supported = undo.destinations.size() == undo.sources.size() &&
                    std::none_of(undo.destinations.begin(), undo.destinations.end(), [](const auto& path) { return path.empty(); });
                undo_.push_back(std::move(undo));
                ++undo_revision_;
            }
        }
        if (item.req.undo_revision && item.req.undo_revision == undo_revision_ && !undo_.empty() &&
            (!deleted_paths.empty() || !recycled_paths.empty() || uncertain)) {
            // Partial deletion cannot safely replay the full inverse either.
            if (uncertain) undo_.back().supported = false;
            else undo_.pop_back();
            ++undo_revision_;
        }
        delete_outcomes_.push_back({item.seq, !deleted_paths.empty() || !recycled_paths.empty(), uncertain});
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
        status.completed_items = deleted_paths.size() + recycled_paths.size();
        status.phase = cancelled ? OpPhase::Cancelled :
            !uncertain && error.empty() && status.completed_items != 0 ? OpPhase::Completed : OpPhase::Failed;
        status.last_error = cancelled ? L"" : std::move(error);
        status.summary = cancelled ? l10n::Get(executed ? l10n::StringId::Cancel : l10n::StringId::DeleteCancelled) :
            status.phase == OpPhase::Completed ? l10n::Get(l10n::StringId::DeleteFinished) : status.last_error;
    });
}

void OpsManager::RunDelete(const QueueItem& item) {
    delete_cancel_.store(false);
    shell_cancel_requested_.store(false);
    delete_active_.store(true);
    if (stopping_.load()) { FinishDelete(item, L"", {}, false, false, {}, {}, true); return; }
    SetStatus([&](OpStatus& status) {
        status.active = true; status.type = item.req.type; status.task_id = item.seq;
        status.phase = OpPhase::Scanning; status.percent = -1;
        status.summary = l10n::Get(l10n::StringId::DeletePreparing);
        status.last_error.clear(); status.current_item.clear();
        status.source_label.clear(); status.destination_label.clear();
        status.total_items = status.completed_items = status.total_bytes = status.transferred_bytes = 0;
        status.bytes_per_second = status.peak_bytes_per_second = 0; status.eta_seconds = 0;
    });
    if (item.req.undo_revision) {
        bool stale = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stale = item.req.undo_revision != undo_revision_ || undo_.empty();
        }
        if (stale) { FinishDelete(item, L"", {}, false, false, {}, {}, true); return; }
    }
    std::wstring error;
    auto plan = BuildDeletePlan(item.req, item.seq, error);
    if (!error.empty()) { FinishDelete(item, std::move(error)); return; }
    const auto prepared = delete_service_.Prepare(std::move(plan));
    if (prepared.decision == DeleteDecision::Blocked) {
        FinishDelete(item, l10n::Get(l10n::StringId::DeleteUnknownStopped)); return;
    }
    if (delete_cancel_.load() || stopping_.load()) delete_service_.Cancel();
    bool no_presenter = false, confirmation_expired = false;
    if (prepared.decision == DeleteDecision::AwaitingConfirmation) {
        // No presenter, shutdown, rejection and timeout all fail closed.
        if (!notify_ || !IsWindow(ui_hwnd_.load())) {
            no_presenter = true;
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
                    confirmation_expired = true;
                    delete_service_.Cancel(); break;
                }
            }
        }
    }
    auto accepted = delete_service_.TakeAccepted(prepared.token);
    if (!accepted || delete_cancel_.load() || stopping_.load()) {
        const bool cancelled = delete_cancel_.load() || stopping_.load() ||
            (!no_presenter && !confirmation_expired && delete_service_.WasRejected(prepared.token));
        FinishDelete(item, l10n::Get(cancelled ? l10n::StringId::DeleteCancelled : l10n::StringId::OperationFailedMessage),
            {}, false, false, {}, {}, cancelled);
        return;
    }
    std::vector<std::wstring> physical, permanent_roots, recycle_roots;
    for (const auto& target : accepted->targets) {
        if (target.disposition != DeleteDisposition::Permanent &&
            target.disposition != DeleteDisposition::RecycleRequested) {
            FinishDelete(item, l10n::Get(l10n::StringId::DeleteUnknownStopped)); return;
        }
        physical.insert(physical.end(), target.physical_paths.begin(), target.physical_paths.end());
        auto& group = target.disposition == DeleteDisposition::Permanent ? permanent_roots : recycle_roots;
        group.insert(group.end(), target.physical_paths.begin(), target.physical_paths.end());
    }
    QueueItem admitted = item;
    admitted.req.sources = physical;
    const bool recycle = !recycle_roots.empty();
    admitted.req.type = recycle ? OpType::RecycleDelete : OpType::RealDelete; // Freeze empty-bin roots.
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
        FinishDelete(item, L"", {}, false, false, {}, {}, true); return;
    }
    SetStatus([&](OpStatus& status) {
        status.phase = OpPhase::Running; status.total_items = accepted->targets.size();
        status.summary = l10n::Get(recycle ? l10n::StringId::Delete : l10n::StringId::PermanentDelete);
    });
    std::vector<std::wstring> actual, recycled, recycle_destinations;
    uint32_t hr = 0;
    bool cancelled = false;
    uint64_t execution_token = prepared.token;
    // One confirmed snapshot, separate backend modes. Never permanently delete
    // the local members of a mixed network/local batch just to suppress Shell UI.
    for (const auto& group : {std::pair{false, permanent_roots}, std::pair{true, recycle_roots}}) {
        if (group.second.empty()) continue;
        if (!execution_token) { hr = static_cast<uint32_t>(E_UNEXPECTED); break; }
        shell_activity_tick_ = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done_ready_ = false; done_deleted_paths_.clear(); done_recycled_paths_.clear(); done_recycle_destinations_.clear();
        }
        auto& client = ipc::ShellClient::Instance();
        uint32_t id = 0;
        {
            // Linearize Cancel/Stop against the destructive send handoff. No mutex
            // is held during confirmation, and no backend retry is allowed.
            std::unique_lock<std::mutex> lock(delete_wait_mutex_);
            if (delete_cancel_.load() || stopping_.load()) {
                lock.unlock();
                cancelled = true; break;
            }
            id = client.DeleteAuthorized(group.second, execution_token, group.first, ui_hwnd_.load());
            current_req_id_.store(id);
        }
        if (delete_cancel_.load() || shell_cancel_requested_.load()) client.Cancel(id);
        if (id == 0) { hr = static_cast<uint32_t>(E_FAIL); break; }
        if (!WaitShellDone(id, hr, cancelled, error)) {
            // WaitShellDone already bumps the finished-task counter. Preserve an
            // uncertain active plan: the backend may have mutated before Stop.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                delete_outcomes_.push_back({item.seq, false, true});
                if (item.req.undo_revision && item.req.undo_revision == undo_revision_ && !undo_.empty()) {
                    undo_.back().supported = false; ++undo_revision_;
                }
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
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            for (const auto& path : done_deleted_paths_)
                if (std::any_of(group.second.begin(), group.second.end(), [&](const auto& requested) { return SamePath(path, requested); })) actual.push_back(path);
            for (size_t i = 0; i < done_recycled_paths_.size(); ++i) {
                const auto& path = done_recycled_paths_[i];
                if (std::any_of(group.second.begin(), group.second.end(), [&](const auto& requested) { return SamePath(path, requested); })) {
                    recycled.push_back(path);
                    recycle_destinations.push_back(i < done_recycle_destinations_.size() ? done_recycle_destinations_[i] : L"");
                }
            }
            done_deleted_paths_.clear(); done_recycled_paths_.clear(); done_recycle_destinations_.clear();
        }
        if (FAILED(static_cast<HRESULT>(hr)) || cancelled) break;
        if (!group.first && !recycle_roots.empty())
            execution_token = delete_service_.ReserveExecutionToken(prepared.token);
    }
    const auto logical = MapDeletedTargets(*accepted, actual);
    const auto logical_recycled = MapDeletedTargets(*accepted, recycled);
    std::vector<std::wstring> logical_destinations;
    for (const auto& path : logical_recycled) {
        const auto target = std::find_if(accepted->targets.begin(), accepted->targets.end(), [&](const auto& value) { return value.path == path; });
        const auto root = target == accepted->targets.end() ? recycled.end() : std::find_if(recycled.begin(), recycled.end(),
            [&](const auto& value) { return SamePath(target->physical_paths.front(), value); });
        const size_t index = static_cast<size_t>(root - recycled.begin());
        logical_destinations.push_back(index < recycle_destinations.size() ? recycle_destinations[index] : L"");
    }
    actual.insert(actual.end(), recycled.begin(), recycled.end());
    const bool physical_complete = IsDeleteSnapshotComplete(*accepted, actual);
    if (cancelled) error = l10n::Get(l10n::StringId::Cancel);
    else if (FAILED(static_cast<HRESULT>(hr)) && error.empty()) error = l10n::Get(l10n::StringId::OperationFailedMessage);
    else if (!physical_complete && error.empty()) error = L"Deletion was not confirmed for every authorized physical root.";
    const bool uncertain = hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE)) ||
        hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED)) ||
        hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)) ||
        error == L"Malformed deletion result" ||
        !physical_complete;
    FinishDelete(item, std::move(error), logical, true, uncertain, logical_recycled, logical_destinations, cancelled);
}
}
