// SPDX-License-Identifier: Apache-2.0
#include "ops_manager_internal.h"
#include "transfer_hash.h"
#include "../common/file_name.h"
#include "../common/file_operation_error.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include <array>
#include <filesystem>

namespace pulse::ops {
namespace {
std::wstring Parent(const std::wstring& path) { return std::filesystem::path(path).parent_path().wstring(); }
std::wstring Leaf(const std::wstring& path) { return std::filesystem::path(path).filename().wstring(); }
std::wstring Join(const std::wstring& parent, const std::wstring& name) {
    return (std::filesystem::path(parent) / name).wstring();
}
bool SamePath(std::wstring a, std::wstring b) {
    std::replace(a.begin(), a.end(), L'/', L'\\'); std::replace(b.begin(), b.end(), L'/', L'\\');
    return !_wcsicmp(a.c_str(), b.c_str());
}
bool SameObjectPath(std::wstring a, std::wstring b) {
    a = fs::NormalizePath(std::move(a)); b = fs::NormalizePath(std::move(b));
    if (a == b) return true;
    if (_wcsicmp(a.c_str(), b.c_str())) return false;
    // Folded names may denote different objects in a case-sensitive directory.
    FileIdentity left, right;
    return ReadPathFileIdentity(a, left) && left.Valid() && ReadPathFileIdentity(b, right) && left == right;
}
bool Within(const std::wstring& child, const std::wstring& parent) {
    auto ancestor = std::filesystem::path(child).lexically_normal().parent_path();
    while (!ancestor.empty()) {
        if (SameObjectPath(ancestor.wstring(), parent)) return true;
        const auto next = ancestor.parent_path();
        if (next == ancestor) break;
        ancestor = next;
    }
    return false;
}
bool Metadata(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& value) {
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &value) != FALSE;
}
bool PlainDirectory(const WIN32_FILE_ATTRIBUTE_DATA& value) {
    return (value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
uint64_t Bytes(const WIN32_FILE_ATTRIBUTE_DATA& value) {
    return (static_cast<uint64_t>(value.nFileSizeHigh) << 32) | value.nFileSizeLow;
}
}

bool OpsManager::DispatchShellOperation(ipc::ShellOperationRequest request, ShellCompletion& result) {
    if (stopping_.load() || shell_cancel_requested_.load() || transfer_cancel_.load()) {
        result.cancelled = true;
        return true;
    }
    request.owner_hwnd = reinterpret_cast<uint64_t>(ui_hwnd_.load());
    request.elevation_authorized = shell_elevation_authorized_;
    shell_activity_tick_ = GetTickCount64();
    auto& client = ipc::ShellClient::Instance();
    const auto id = client.RunFileOperation(request, ShellOwner(), [this](uint32_t value) {
        current_req_id_.store(value);
    });
    if (!id) { result.hr = static_cast<uint32_t>(E_FAIL); return true; }
    if (transfer_pause_.load()) client.SetFileOperationPaused(id, true);
    if (stopping_.load() || shell_cancel_requested_.load() || transfer_cancel_.load()) client.Cancel(id);
    return WaitShellDone(id, result);
}

void OpsManager::RunBasicShellOperation(const OpRequest& req, uint64_t task_id,
    const FileIdentity* rename_identity, const std::vector<FileIdentity>* continuation_identities) {
    transfer_cancel_.store(stopping_.load() || lock_retry_cancel_.load());
    transfer_pause_.store(false);
    const bool creation = req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile;
    HRESULT failure = S_OK;
    HRESULT first_failure = S_OK;
    std::wstring failed_path;
    std::wstring first_failed_path;
    bool cancelled = false;
    OpRequest completed = req;
    completed.sources.clear();
    completed.undo_produced_identities.clear();
    std::vector<std::wstring> destinations;
    std::vector<FileIdentity> identities;
    std::vector<FileObjectReference> references;
    std::vector<FileObjectReference> source_references;
    shell_progress_total_.store(req.sources.size());
    struct FinishProgress { OpsManager& owner; ~FinishProgress() { owner.shell_progress_total_.store(0); } } finish_progress{*this};
    if (!creation) for (const auto& source : req.sources) source_references.push_back(FileObjectReference::Open(source));
    if (req.sources.empty() || (req.type != OpType::BatchRename && req.sources.size() != 1)) failure = E_INVALIDARG;
    for (size_t i = 0; i < req.sources.size(); ++i) {
        if (FAILED(failure)) {
            if (SUCCEEDED(first_failure)) { first_failure = failure; first_failed_path = failed_path; }
            if (req.type != OpType::BatchRename || !req.is_undo) break;
            failure = S_OK;
        }
        if (stopping_.load() || shell_cancel_requested_.load()) { cancelled = true; break; }
        shell_progress_base_.store(completed.sources.size());
        const auto name = req.type == OpType::BatchRename && i < req.new_names.size() ? req.new_names[i] : req.new_name;
        if (!creation && !path::IsValidFileName(name)) { failure = HRESULT_FROM_WIN32(ERROR_INVALID_NAME); break; }
        ipc::ShellOperationRequest operation;
        operation.kind = creation ? (req.type == OpType::CreateFolder ? ipc::ShellOperationKind::CreateFolder
            : ipc::ShellOperationKind::CreateFile) : ipc::ShellOperationKind::Rename;
        ipc::ShellOperationItem item;
        item.source = creation ? L"" : req.sources[i];
        item.destination = creation ? req.sources[i] : Join(Parent(req.sources[i]), name);
        if (!creation) {
            if (req.is_undo) {
                if (i >= req.undo_produced_identities.size() || !req.undo_produced_identities[i].Valid()) { failure = E_ACCESSDENIED; break; }
                item.expected_identity = req.undo_produced_identities[i];
            }
            else if (rename_identity) item.expected_identity = *rename_identity;
            else if (continuation_identities && i < continuation_identities->size()) item.expected_identity = (*continuation_identities)[i];
        }
        operation.items.push_back(item);
        ShellCompletion result;
        if (!DispatchShellOperation(std::move(operation), result)) return;
        cancelled = result.cancelled;
        failure = static_cast<HRESULT>(result.hr);
        failed_path = result.failed_path;
        const auto found = std::find_if(result.shell_items.begin(), result.shell_items.end(), [&](const auto& value) {
            return SamePath(value.source, item.source) && SUCCEEDED(value.hr) && !value.skipped;
        });
        if (found != result.shell_items.end()) {
            auto reference = FileObjectReference::Open(found->destination);
            if (req.folder_metadata && req.type == OpType::CreateFolder) {
                HANDLE folder = CreateFileW(found->destination.c_str(), FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (folder == INVALID_HANDLE_VALUE) failure = HRESULT_FROM_WIN32(GetLastError());
                else {
                    if (!reference.Matches(folder) || !SetFileInformationByHandle(folder, FileBasicInfo,
                        const_cast<FILE_BASIC_INFO*>(&*req.folder_metadata), sizeof(FILE_BASIC_INFO)))
                        failure = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_FILE_INVALID);
                    CloseHandle(folder);
                }
            }
            completed.sources.push_back(req.sources[i]);
            if (req.is_undo) completed.undo_produced_identities.push_back(item.expected_identity);
            destinations.push_back(found->destination);
            identities.push_back(reference.identity);
            references.push_back(std::move(reference));
        } else if (SUCCEEDED(failure) && !cancelled) failure = E_FAIL;
        if (FAILED(failure) && !cancelled && found == result.shell_items.end()) {
            QueueItem retry;
            retry.seq = task_id; retry.req = req;
            retry.req.sources.erase(retry.req.sources.begin(), retry.req.sources.begin() + i);
            if (req.type == OpType::BatchRename && retry.req.new_names.size() >= i)
                retry.req.new_names.erase(retry.req.new_names.begin(), retry.req.new_names.begin() + i);
            if (!creation) retry.retry_source_references.assign(source_references.begin() + i, source_references.end());
            if (req.type == OpType::Rename && source_references[i].Get()) {
                retry.retry_rename_identity = true; retry.rename_source_id = source_references[i].identity;
            }
            const auto locks = creation ? LockReport{} :
                CaptureLockedFailure(retry, failure, failed_path, [&] { return stopping_.load(); });
            if (locks.owners.empty() && !IsLockFailure(failure)) CaptureElevationFailure(retry, failure, failed_path);
            SetStatus([&](OpStatus& status) {
                status.locked_path = locks.path; status.lock_owners = locks.owners;
                status.lock_retry_available = lock_retry_.has_value();
            });
        }
        SetStatus([&](OpStatus& status) { status.completed_items = completed.sources.size(); });
        if (cancelled) break;
    }
    if (FAILED(first_failure)) { failure = first_failure; failed_path = first_failed_path; }
    if (!completed.sources.empty()) PushUndo(completed, &destinations, &identities, &references);
    SetStatus([&](OpStatus& status) {
        status.task_id = task_id; status.active = false; ++status.completed_ops;
        status.phase = cancelled ? OpPhase::Cancelled : FAILED(failure) ? OpPhase::Failed : OpPhase::Completed;
        status.failure_hr = cancelled ? S_OK : failure;
        status.failed_path = failed_path;
        status.last_error = FAILED(failure) && !cancelled ? format::FileOperationErrorText(failure) : L"";
        status.summary = cancelled ? l10n::Get(l10n::StringId::OperationCancelled)
            : FAILED(failure) ? status.last_error : l10n::Get(l10n::StringId::OpCompleted);
        status.percent = SUCCEEDED(failure) && !cancelled ? 100.0f : -1.0f;
    });
}

void OpsManager::RunTransfer(const OpRequest& req, uint64_t task_id,
    const std::vector<TransferPlanEntry>& prior_plan) {
    transfer_active_.store(true);
    transfer_cancel_.store(stopping_.load() || lock_retry_cancel_.load());
    transfer_pause_.store(false);
    shell_bytes_base_.store(0);
    shell_cancel_requested_.store(transfer_cancel_.load());
    struct Finish { OpsManager& owner; ~Finish() {
        owner.transfer_active_.store(false); owner.shell_progress_total_.store(0);
        std::lock_guard lock(owner.transfer_control_mutex_); owner.pending_conflict_.reset();
    }} finish{*this};
    const auto cancelled = [&] { return stopping_.load() || transfer_cancel_.load() || shell_cancel_requested_.load(); };
    SetStatus([&](OpStatus& status) {
        status.type = req.type; status.task_id = task_id; status.active = true; status.phase = OpPhase::Scanning;
        status.source_label = req.sources.empty() ? L"" : req.sources.front(); status.destination_label = req.dest_dir;
        status.percent = -1.0f; status.total_items = status.completed_items = 0;
        status.total_bytes = status.transferred_bytes = 0; status.bytes_per_second = status.peak_bytes_per_second = 0;
        status.eta_seconds = 0; status.last_error.clear(); status.failure_hr = S_OK;
        shell_transfer_rate_.Reset(GetTickCount64(), 0);
        status.summary = l10n::Format(l10n::StringId::OperationPreparingFormat,
            l10n::Get(req.type == OpType::Copy ? l10n::StringId::Copy : l10n::StringId::OperationMove).c_str());
    });
    struct Work {
        TransferPlanEntry entry;
        bool conflict = false;
        bool preexisting = false;
        ConflictChoice choice = ConflictChoice::KeepBoth;
        WIN32_FILE_ATTRIBUTE_DATA target{};
        FileObjectReference target_identity;
    };
    std::vector<Work> work;
    std::vector<TransferPlanEntry> merged_directories;
    HRESULT failure = S_OK;
    std::wstring failed_path;
    const auto fail = [&](DWORD code, const std::wstring& path) { failure = HRESULT_FROM_WIN32(code); failed_path = path; };
    std::function<bool(const std::wstring&)> prepare_undo_parent;
    prepare_undo_parent = [&](const std::wstring& directory) {
        WIN32_FILE_ATTRIBUTE_DATA metadata{};
        if (Metadata(directory, metadata)) {
            if (metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return true;
            fail(ERROR_DIRECTORY, directory); return false;
        }
        const DWORD error = GetLastError();
        const auto parent = Parent(directory);
        if ((error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND) || directory.empty() || parent == directory) {
            fail(error, directory); return false;
        }
        if (!prepare_undo_parent(parent)) return false;
        ipc::ShellOperationRequest operation;
        operation.kind = ipc::ShellOperationKind::CreateFolder;
        operation.items.push_back({L"", directory, {}, {}});
        ShellCompletion result;
        if (!DispatchShellOperation(std::move(operation), result)) { failure = E_ABORT; return false; }
        if (result.cancelled) transfer_cancel_.store(true);
        if (FAILED(static_cast<HRESULT>(result.hr))) {
            failure = static_cast<HRESULT>(result.hr); failed_path = directory; return false;
        }
        return !cancelled();
    };
    std::function<void(const std::wstring&, const std::wstring&)> plan;
    plan = [&](const std::wstring& source, const std::wstring& target) {
        if (cancelled() || FAILED(failure)) return;
        WIN32_FILE_ATTRIBUTE_DATA from{}, to{};
        if (!Metadata(source, from)) { fail(GetLastError(), source); return; }
        const bool exists = Metadata(target, to);
        const DWORD target_error = exists ? ERROR_SUCCESS : GetLastError();
        if (!exists && target_error != ERROR_FILE_NOT_FOUND && target_error != ERROR_PATH_NOT_FOUND) {
            fail(target_error, target); return;
        }
        TransferPlanEntry entry;
        entry.source = source; entry.destination = target;
        entry.directory = (from.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.reparse = (from.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        entry.bytes = entry.directory ? 0 : Bytes(from); entry.modified = from.ftLastWriteTime;
        // Optional during ordinary work; required only when continuing an old request.
        entry.source_identity = FileObjectReference::Open(source);
        const bool same_object = exists && SameObjectPath(source, target);
        if (PlainDirectory(from) && exists && PlainDirectory(to) && !same_object) {
            merged_directories.push_back(entry);
            try {
                fs::DirectoryEnumerator cursor(source);
                fs::DirEntry child;
                while (cursor.Next(child, cancelled) == fs::DirectoryRead::Entry)
                    plan(Join(source, child.name), Join(target, child.name));
            } catch (const fs::EnumerationError& error) { fail(error.Code(), source); }
            return;
        }
        if (same_object && req.type == OpType::Move) return;
        Work next{entry, exists && !same_object, exists, ConflictChoice::KeepBoth, to, {}};
        if (next.conflict) next.target_identity = FileObjectReference::Open(target);
        next.choice = req.collision_policy == CollisionPolicy::Replace && next.conflict
            ? ConflictChoice::Replace : ConflictChoice::KeepBoth;
        work.push_back(std::move(next));
    };
    if (req.sources.empty() || req.dest_dir.empty()) failure = E_INVALIDARG;
    if (SUCCEEDED(failure)) {
        if (!prior_plan.empty()) {
            for (const auto& entry : prior_plan) {
                const auto current = FileObjectReference::Open(entry.source);
                WIN32_FILE_ATTRIBUTE_DATA metadata{};
                if (!entry.source_identity.Matches(current.Get()) || !Metadata(entry.source, metadata) ||
                    (!entry.directory && (Bytes(metadata) != entry.bytes ||
                        CompareFileTime(&entry.modified, &metadata.ftLastWriteTime)))) {
                    fail(ERROR_FILE_INVALID, entry.source); break;
                }
                plan(entry.source, entry.destination);
            }
        } else for (size_t i = 0; i < req.sources.size(); ++i) {
            const auto& source = req.sources[i];
            if (source.empty()) { fail(ERROR_INVALID_PARAMETER, source); break; }
            const auto target = req.type == OpType::Move && req.is_undo && i < req.new_names.size()
                ? req.new_names[i] : Join(req.dest_dir, Leaf(source));
            if (req.is_undo) {
                FileIdentity current;
                if (i >= req.undo_produced_identities.size() || !ReadPathFileIdentity(source, current) ||
                    current != req.undo_produced_identities[i]) { fail(ERROR_FILE_INVALID, source); break; }
                if (std::find(req.undo_cleanup_directories.begin(), req.undo_cleanup_directories.end(), source) !=
                    req.undo_cleanup_directories.end()) {
                    WIN32_FILE_ATTRIBUTE_DATA destination{};
                    if (!Metadata(target, destination) || !(destination.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        fail(ERROR_PATH_NOT_FOUND, target); break;
                    }
                    TransferPlanEntry cleanup;
                    cleanup.source = source; cleanup.destination = target; cleanup.directory = true;
                    cleanup.source_identity = FileObjectReference::Open(source);
                    merged_directories.push_back(std::move(cleanup));
                    continue;
                }
                if (!prepare_undo_parent(Parent(target))) break;
            }
            if (SameObjectPath(source, req.dest_dir) || Within(req.dest_dir, source)) {
                fail(ERROR_INVALID_PARAMETER, target); break;
            }
            plan(source, target);
        }
    }
    const bool plan_complete = SUCCEEDED(failure) && !cancelled();
    if (plan_complete && req.is_undo && req.type == OpType::Move && !merged_directories.empty()) {
        UndoEntry remaining;
        const auto retain = [&](const TransferPlanEntry& entry) {
            if (!entry.source_identity.Get()) { fail(ERROR_FILE_INVALID, entry.source); return; }
            remaining.sources.push_back(entry.destination); remaining.destinations.push_back(entry.source);
            remaining.produced_identities.push_back(entry.source_identity.identity);
            remaining.produced_references.push_back(entry.source_identity);
        };
        for (const auto& item : work) retain(item.entry);
        for (const auto& entry : merged_directories) {
            retain(entry); remaining.cleanup_directories.push_back(entry.source);
        }
        if (SUCCEEDED(failure)) {
            std::lock_guard lock(mutex_);
            if (req.undo_revision != undo_revision_ || undo_.empty()) failure = E_ACCESSDENIED;
            else {
                auto& history = undo_.back();
                history.sources = std::move(remaining.sources); history.destinations = std::move(remaining.destinations);
                history.produced_identities = std::move(remaining.produced_identities);
                if (history.session_only) history.produced_references = std::move(remaining.produced_references);
                else history.produced_references.clear();
                history.cleanup_directories = std::move(remaining.cleanup_directories);
            }
        }
    }
    if (plan_complete && req.collision_policy == CollisionPolicy::System) {
        ConflictItemInfo info;
        info.task_id = task_id;
        for (size_t i = 0; i < work.size(); ++i) if (work[i].conflict) {
            const auto& item = work[i];
            info.rows.push_back({i + 1, item.entry.source, item.entry.destination, item.entry.bytes, Bytes(item.target),
                item.entry.modified, item.target.ftLastWriteTime, item.entry.directory,
                (item.target.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, item.entry.reparse,
                (item.target.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
        }
        if (!info.rows.empty()) {
            const auto& first = info.rows.front();
            info.source = first.source; info.destination = first.destination;
            info.source_size = first.source_size; info.destination_size = first.destination_size;
            info.source_modified = first.source_modified; info.destination_modified = first.destination_modified;
            info.source_is_directory = first.source_is_directory; info.destination_is_directory = first.destination_is_directory;
            info.remaining = info.rows.size();
            {
                std::lock_guard lock(transfer_control_mutex_);
                info.token = next_conflict_token_++;
                info.still_valid = [this, token = info.token] {
                    std::lock_guard lock(transfer_control_mutex_);
                    return pending_conflict_ && pending_conflict_->token == token && !transfer_cancel_.load() && !stopping_.load();
                };
                pending_conflict_ = info; resolved_conflict_token_ = 0; resolved_conflict_rows_.clear();
            }
            SetStatus([&](OpStatus& status) {
                status.phase = OpPhase::WaitingForConflict;
                status.summary = l10n::Get(l10n::StringId::OperationWaitingConflict);
            });
            std::unique_lock lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] { return cancelled() || resolved_conflict_token_ == info.token; });
            if (!cancelled()) {
                if (!resolved_conflict_rows_.empty()) {
                    for (const auto& row : resolved_conflict_rows_) work[row.row_id - 1].choice = row.choice;
                } else {
                    for (const auto& row : info.rows) work[row.row_id - 1].choice = resolved_conflict_choice_;
                    if (resolved_conflict_choice_ == ConflictChoice::Cancel) transfer_cancel_.store(true);
                }
            }
            pending_conflict_.reset(); resolved_conflict_rows_.clear();
        }
    }
    TransferOutcome outcome;
    outcome.task_id = task_id; outcome.type = req.type; outcome.plan_complete = plan_complete;
    OpRequest completed = req; completed.sources.clear(); completed.undo_produced_identities.clear();
    std::vector<std::wstring> destinations;
    std::vector<FileIdentity> identities;
    std::vector<FileObjectReference> references;
    shell_progress_total_.store(work.size());
    uint64_t completed_bytes = 0;
    SetStatus([&](OpStatus& status) {
        for (const auto& item : work) if (item.choice != ConflictChoice::Skip) status.total_bytes += item.entry.bytes;
    });
    size_t processed = 0;
    std::vector<TransferPlanEntry> unstarted;
    for (size_t i = 0; i < work.size(); ++i) {
        auto& item = work[i];
        const auto& entry = item.entry;
        TransferItemResult state{entry.source, entry.destination, entry.directory, entry.reparse};
        if (transfer_pause_.load() && !cancelled()) {
            std::unique_lock lock(transfer_control_mutex_);
            transfer_control_cv_.wait(lock, [&] { return !transfer_pause_.load() || cancelled(); });
        }
        if (FAILED(failure) || cancelled()) {
            if (entry.source_identity.Get()) unstarted.push_back(entry);
            outcome.items.push_back(std::move(state)); continue;
        }
        shell_progress_base_.store(processed);
        shell_bytes_base_.store(completed_bytes);
        SetStatus([&](OpStatus& status) {
            status.phase = OpPhase::Running; status.total_items = work.size(); status.completed_items = processed;
            status.current_item = entry.source; status.summary = entry.source;
        });
        if (item.choice == ConflictChoice::Skip) {
            state.state = TransferItemState::Skipped; ++processed; outcome.items.push_back(std::move(state)); continue;
        }
        if (cancelled()) { unstarted.push_back(entry); outcome.items.push_back(std::move(state)); continue; }
        ipc::ShellOperationRequest operation;
        operation.kind = req.type == OpType::Move ? ipc::ShellOperationKind::Move : ipc::ShellOperationKind::Copy;
        operation.collision = req.is_undo ? ipc::ShellCollisionPolicy::Fail
            : item.choice == ConflictChoice::Replace ? ipc::ShellCollisionPolicy::Replace : ipc::ShellCollisionPolicy::KeepBoth;
        ipc::ShellOperationItem source{entry.source, entry.destination, {}, {}};
        if (item.choice == ConflictChoice::Replace) {
            source.expected_destination_identity = item.target_identity.identity;
            // Even SHARE_DELETE keeps a removed target pending until close, blocking Shell replacement.
            // Keep the confirmed ID for the Shell callback, but release our target handle before mutation.
            item.target_identity.handle.reset();
        }
        if (req.is_undo) {
            source.expected_identity = entry.source_identity.identity;
            if (!source.expected_identity.Valid()) { fail(ERROR_FILE_INVALID, entry.source); outcome.items.push_back(state); continue; }
        }
        operation.items.push_back(source);
        ShellCompletion result;
        if (!DispatchShellOperation(std::move(operation), result)) return;
        if (result.cancelled) transfer_cancel_.store(true);
        uint64_t produced_bytes = 0;
        for (const auto& output : result.shell_items) {
            WIN32_FILE_ATTRIBUTE_DATA metadata{};
            if (SUCCEEDED(output.hr) && !output.skipped && Metadata(output.destination, metadata) &&
                !(metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) produced_bytes += Bytes(metadata);
        }
        completed_bytes += produced_bytes;
        SetStatus([&](OpStatus& status) {
            status.transferred_bytes = (std::max)(status.transferred_bytes, completed_bytes);
            status.total_bytes = (std::max)(status.total_bytes, status.transferred_bytes);
        });
        const auto root = std::find_if(result.shell_items.begin(), result.shell_items.end(), [&](const auto& value) {
            return SamePath(value.source, entry.source);
        });
        if (root != result.shell_items.end() && SUCCEEDED(root->hr) && !root->skipped &&
            (!entry.directory || (SUCCEEDED(static_cast<HRESULT>(result.hr)) && !result.cancelled))) {
            state.destination = root->destination; state.destination_committed = true;
            state.source_removed = req.type == OpType::Move; state.state = TransferItemState::Completed;
            completed.sources.push_back(entry.source); destinations.push_back(root->destination);
            if (req.is_undo) completed.undo_produced_identities.push_back(source.expected_identity);
            auto reference = FileObjectReference::Open(root->destination);
            identities.push_back(item.preexisting && item.choice == ConflictChoice::Replace ? FileIdentity{} : reference.identity);
            references.push_back(std::move(reference));
            ++processed;
        } else if (root != result.shell_items.end() && root->skipped && SUCCEEDED(root->hr)) {
            state.state = TransferItemState::Skipped; ++processed;
        }
        if (FAILED(static_cast<HRESULT>(result.hr))) {
            failure = static_cast<HRESULT>(result.hr);
            failed_path = result.failed_path.empty() ? entry.source : result.failed_path;
            const bool untouched_cancel = result.cancelled && !entry.directory && !item.preexisting &&
                GetFileAttributesW(entry.destination.c_str()) == INVALID_FILE_ATTRIBUTES &&
                (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
            const auto code = format::FileOperationMessageCode(failure);
            const bool permission_failure = code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ||
                code == ERROR_ELEVATION_REQUIRED;
            if ((IsLockFailure(failure) || permission_failure || untouched_cancel) && entry.source_identity.Get() &&
                std::none_of(result.shell_items.begin(), result.shell_items.end(), [](const auto& value) {
                    return SUCCEEDED(value.hr) && !value.skipped;
                })) unstarted.push_back(entry);
        } else if (state.state == TransferItemState::Incomplete && !cancelled()) fail(ERROR_GEN_FAILURE, entry.source);
        if (verify_copies_.load() && req.type == OpType::Copy && !cancelled() && SUCCEEDED(failure)) {
            SetStatus([](OpStatus& status) { status.phase = OpPhase::Verifying; });
            for (const auto& output : result.shell_items) {
                const DWORD attributes = GetFileAttributesW(output.source.c_str());
                if (FAILED(output.hr) || output.skipped || attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
                std::array<uint8_t, 32> a{}, b{};
                std::wstring error;
                if (!Sha256File(output.source, transfer_cancel_, transfer_pause_, a, error) ||
                    !Sha256File(output.destination, transfer_cancel_, transfer_pause_, b, error) || a != b) {
                    fail(ERROR_CRC, output.destination); break;
                }
            }
        }
        // A failed subtree never authorizes undo or replay of the entire root.
        if (state.state == TransferItemState::Incomplete) {
            for (const auto& child : result.shell_items) {
                if (SamePath(child.source, entry.source) || FAILED(child.hr) || child.skipped || child.destination.empty()) continue;
                const auto attrs = GetFileAttributesW(child.destination.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
                outcome.items.push_back({child.source, child.destination, false, false,
                    TransferItemState::Completed, true, req.type == OpType::Move});
                completed.sources.push_back(child.source); destinations.push_back(child.destination);
                auto reference = FileObjectReference::Open(child.destination);
                identities.push_back(item.preexisting ? FileIdentity{} : reference.identity);
                references.push_back(std::move(reference));
            }
        }
        outcome.items.push_back(std::move(state));
    }
    // Merged source containers are removed only when empty, never recursively.
    if (req.type == OpType::Move && SUCCEEDED(failure) && !cancelled()) {
        for (auto it = merged_directories.rbegin(); it != merged_directories.rend(); ++it) {
            if (!it->source_identity.Get()) continue;
            DWORD error = ERROR_SUCCESS;
            if (!it->source_identity.Delete(it->source, error)) {
                if (req.is_undo || (error != ERROR_DIR_NOT_EMPTY && error != ERROR_PATH_NOT_FOUND && error != ERROR_FILE_NOT_FOUND))
                    fail(error, it->source);
            } else if (req.is_undo) {
                completed.sources.push_back(it->source); destinations.push_back(it->destination);
                completed.undo_produced_identities.push_back(it->source_identity.identity);
                identities.push_back({}); references.push_back({});
            }
        }
    }
    if (!completed.sources.empty()) PushUndo(completed, &destinations, &identities, &references);
    for (const auto& item : outcome.items)
        if (item.state == TransferItemState::Incomplete) outcome.remaining_sources.push_back(item.source);
    outcome.can_resume = !req.is_undo && !unstarted.empty();
    LockReport locks;
    if (!req.is_undo && FAILED(failure) && !cancelled() && !unstarted.empty()) {
        QueueItem retry;
        retry.req = req; retry.seq = task_id; retry.retry_transfer_plan = unstarted;
        locks = CaptureLockedFailure(retry, failure, failed_path, cancelled);
        if (locks.owners.empty() && !IsLockFailure(failure)) {
            retry.req.sources.clear();
            for (const auto& entry : unstarted) retry.req.sources.push_back(entry.source);
            CaptureElevationFailure(std::move(retry), failure, failed_path);
        }
    }
    SetStatus([&](OpStatus& status) {
        status.active = false; ++status.completed_ops; status.completed_items = processed; status.total_items = work.size();
        status.phase = cancelled() ? OpPhase::Cancelled : FAILED(failure) ? OpPhase::Failed : OpPhase::Completed;
        status.failure_hr = cancelled() ? S_OK : failure; status.failed_path = failed_path;
        status.locked_path = locks.path; status.lock_owners = locks.owners;
        status.lock_retry_available = lock_retry_.has_value();
        status.last_error = FAILED(failure) && !cancelled() ? format::FileOperationErrorText(failure) + L" | " + failed_path : L"";
        status.summary = cancelled() ? l10n::Get(l10n::StringId::OperationCancelled)
            : FAILED(failure) ? status.last_error : l10n::Get(l10n::StringId::OpCompleted);
        if (!cancelled() && SUCCEEDED(failure)) status.percent = 100.0f;
        outcome.phase = status.phase; outcome.failure_hr = status.failure_hr;
        if (outcome.can_resume) {
            QueueItem continuation;
            continuation.req = req; continuation.seq = task_id;
            continuation.retry_transfer_plan = std::move(unstarted);
            incomplete_transfers_.push_back(std::move(continuation));
            if (incomplete_transfers_.size() > 16) incomplete_transfers_.pop_front();
        }
        transfer_outcomes_.push_back(std::move(outcome));
    });
}
}
