// ops_manager.h — Operation queue, progress status, undo stack (UI-process side).
//
// Queued operations are serialized on the operation worker. Copy/move/rename
// execute in-process with Win32 APIs; authorized delete/recycle, restore and
// creation use Pulse.Shell.exe (ShellClient). Context menus have a separate
// client worker. Submit/Cancel/Undo/Status never run blocking operations on the UI.
//
// Undo model (stage 1B-1):
//   Move          -> undo = move the copies at dest back to their original parent
//   Rename        -> undo = rename back to the original name
//   Copy          -> undo = recycle-delete the produced copies (Explorer-like)
//   CreateFolder /
//   CreateTextFile-> undo = recycle-delete the created item
//   RecycleDelete -> undo = restore the original paths from $Recycle.Bin
//   RealDelete    -> never recorded, not undoable.
#pragma once
#include "delete_service.h"
#include "destination_guard.h"
#include "file_lock_owner.h"
#include "recycle_undo_validation.h"
#include "file_object_reference.h"
#include "../common/shell_menu_item.h"
#include "../ipc/shell_client.h"
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops {

struct DeleteIntegrationProbe;
struct OperationJournalProbe;

enum class OpType { Copy, Move, RecycleDelete, RealDelete, Rename, CreateFolder, CreateTextFile, RestoreRecycle, EmptyRecycle, BatchRename };
enum class CollisionPolicy { System, Replace, KeepBoth };
enum class OpPhase { Queued, Scanning, WaitingForConflict, Running, Paused,
                     Verifying, Cancelling, Completed, Failed, WaitingForDeleteConfirmation, Cancelled };
enum class ConflictChoice { Cancel, Replace, Skip, KeepBoth, Continue };

struct ConflictItemInfo {
    uint64_t token = 0;
    uint64_t task_id = 0;
    std::wstring source;
    std::wstring destination;
    uint64_t source_size = 0;
    uint64_t destination_size = 0;
    FILETIME source_modified{};
    FILETIME destination_modified{};
    bool source_is_directory = false;
    bool destination_is_directory = false;
    size_t remaining = 0;
    bool link_confirmation = false;
    DestinationLinkImpact link_impact;
    std::function<bool()> still_valid; // status/token only; never filesystem work
};

struct DeleteRequestTarget {
    std::wstring path;
    std::vector<std::wstring> physical_paths;
    bool recycle_item = false; // resolve an existing $I companion on the worker
};

struct OpRequest {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::wstring dest_dir;    // Copy / Move
    std::wstring new_name;    // Rename
    std::vector<std::wstring> new_names; // BatchRename names / undo Move full original paths
    CollisionPolicy collision_policy = CollisionPolicy::System; // Copy / Move
    bool is_undo = false;     // undo-originated ops do not re-enter the stack
    uint64_t undo_revision = 0; // reservation for inverses; never serialized
    DeleteOrigin delete_origin = DeleteOrigin::Selection;
    // Logical rows may own more than one physical root (Recycle Bin $R/$I).
    // These describe targets, never authorization or a recyclability claim.
    std::vector<DeleteRequestTarget> delete_targets;
    std::vector<std::wstring> recycle_paths; // exact selected recycle payloads; also used by Undo
    std::vector<RecycleUndoIdentity> recycle_undo_identities; // in-session worker proof, never persisted
    std::vector<FileIdentity> undo_produced_identities; // historical identities aligned with inverse sources
    std::vector<std::wstring> undo_cleanup_directories; // pending empty-directory cleanup; never rescan descendants
};

// Worker snapshot used only for an in-session retry; never journaled. Keeping
// exact descendants and destinations avoids rescanning unrelated new files.
struct TransferPlanEntry {
    std::wstring source;
    std::wstring destination;
    bool directory = false;
    bool reparse = false;
    bool destination_preexisting = false;
    bool directory_rename_only = false; // same-volume root admitted without retaining child handles
    uint64_t bytes = 0;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME modified{};
    FileObjectReference source_identity;
};

struct UndoEntry {
    OpType type = OpType::Copy;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations; // actual names, including keep-both results
    std::wstring dest_dir;
    std::wstring new_name;
    bool supported = true;    // false = recorded for history, cannot be undone
    bool recycle_verified = false; // worker-only cache; never trusted from persisted JSON
    std::vector<RecycleUndoIdentity> recycle_identities;
    std::vector<FileIdentity> produced_identities;
    std::vector<std::wstring> cleanup_directories; // known content already restored; only delete these empty directories
};

struct RestoreItemResult {
    std::wstring original_path;
    std::wstring recycle_path;
    std::wstring destination;
    bool restored = false;
    HRESULT error = S_OK;
};

struct OpStatus {
    bool active = false;
    OpType type = OpType::Copy;
    OpPhase phase = OpPhase::Completed;
    uint64_t task_id = 0;
    float percent = -1.0f;    // <0 = nothing to show
    std::wstring summary;     // one-line status-bar text
    std::wstring last_error;
    std::wstring source_label;
    std::wstring destination_label;
    std::wstring current_item;
    uint64_t total_bytes = 0;
    uint64_t transferred_bytes = 0;
    uint64_t total_items = 0;
    uint64_t completed_items = 0;
    double bytes_per_second = 0.0;
    double peak_bytes_per_second = 0.0;
    uint64_t eta_seconds = 0;
    uint64_t completed_ops = 0; // bumped on every finished op (UI edge detect)
    uint64_t deletes_without_mutation = 0; // suppress refusal-only model refresh
    HRESULT failure_hr = S_OK;
    std::wstring failed_path; // exact failing endpoint, independent of display text
    bool elevation_retry_available = false;
    std::wstring elevation_source_path;
    std::wstring elevation_destination_path;
    bool lock_retry_available = false; // single rename can retry without Restart Manager owners
    std::vector<RestoreItemResult> restore_results; // exact selected payload and actual per-item outcome
    std::wstring locked_path;
    std::vector<LockOwner> lock_owners;
};

struct DeleteOutcome {
    uint64_t task_id = 0;
    bool mutated = false; // at least one confirmed logical target, not proof of zero side effects
    bool uncertain = false; // attempted plan with incomplete/lost backend evidence
};

struct CompletedOperation {
    OpType type = OpType::Copy;
    uint64_t task_id = 0;
    std::vector<std::wstring> sources;
    std::vector<std::wstring> destinations;
    std::optional<std::wstring> volume_label;
};

struct RecoveryEntry {
    uint64_t sequence = 0;
    OpRequest request;
    bool was_active = false;
    bool outcome_unknown = false; // no terminal evidence; retain for inspection without replay
};

struct RecoverySnapshot {
    std::vector<RecoveryEntry> entries;
    bool has_uncertain_destructive = false;
};

using ShellMenuItem = pulse::ShellMenuItem;

class OpsManager {
public:
    OpsManager();
    ~OpsManager();

    // notify is invoked from worker/IPC threads whenever status changes.
    void Start(std::function<void()> notify);
    void Stop();
    bool HasPendingFileOperations() const;
    void BeginShutdown();

    void SetJournalPath(std::wstring path);
    // First-window recovery owner only: load/retain entries from several
    // per-window journals while routing new work to the first (legacy) path.
    void SetJournalPaths(std::vector<std::wstring> paths);
    RecoverySnapshot PendingRecovery() const;
    bool RetryRecovery();
    void DiscardRecovery();
    void SetVerifyCopies(bool enabled) noexcept { verify_copies_.store(enabled); }
    bool VerifyCopies() const noexcept { return verify_copies_.load(); }
    // Owner window for the shell dialogs the open thread raises (打开方式…,
    // 属性). Explorer parents those to the folder window; passing our own HWND
    // keeps them tied to Pulse instead of whatever happens to be foreground
    // when the ops thread gets to the request.
    void SetUiWindow(HWND hwnd) noexcept;
    HWND UiWindow() const noexcept { return ui_hwnd_.load(); }

    uint64_t Submit(OpRequest req);
    void CancelCurrent();
    void PauseCurrent();
    void ResumeCurrent();
    std::optional<ConflictItemInfo> PendingConflict() const;
    void ResolveConflict(uint64_t token, ConflictChoice choice, bool apply_to_all);
    bool IsLockedFailureCurrent(uint64_t task_id) const;
    bool RetryLockedOperation(uint64_t task_id, bool end_owners);
    bool IsElevationFailureCurrent(uint64_t task_id) const;
    bool RetryElevatedOperation(uint64_t task_id);
    void CancelElevationRetry(uint64_t task_id);
    std::optional<DeleteConfirmation> PendingDeleteConfirmation() const;
    void ResolveDeleteConfirmation(uint64_t token, bool accepted);
    std::vector<DeleteOutcome> DrainDeleteOutcomes();

    // Queued Shell launch; execution lives in an isolated Pulse.Shell session.
    void OpenWith(const std::wstring& path);
    void RevealInExplorer(const std::wstring& path);
    std::vector<std::wstring> DrainShellLaunchErrors();

    // Shell properties in an isolated session with its own window lifetime.
    void ShowProperties(const std::wstring& path);
    // Several items: one merged Explorer sheet (SHMultiFileProperties) on a
    // session process that pumps messages while the sheet is open.
    void ShowProperties(const std::vector<std::wstring>& paths);

    // Any registry shell verb ("print", "edit", …) in the Shell session.
    void ExecuteVerb(const std::wstring& path, const std::wstring& verb);

    // Open `file` with a specific application (open-with MRU entry).
    void OpenWithApp(const std::wstring& app_exe, const std::wstring& file);

    // Expand a registry command template (%1/%L) and launch it on the open thread.
    void ExecuteCommand(const std::wstring& command, const std::wstring& path);

    // `wt.exe -d <dir>` in the Shell session.
    void OpenTerminal(const std::wstring& dir);

    // --- Explorer context-menu sessions (pulse_shell IContextMenu) ---------
    // Runs on a dedicated forwarding thread so a slow pipe reconnect or an
    // in-flight transfer never delays a right-click. The callback fires on the
    // shell client's reader thread; PostMessage from it, do not paint.
    using ShellMenuCallback =
        std::function<void(uint32_t token, std::vector<ShellMenuItem> items, bool partial,
                           std::vector<std::wstring> slow_clsids)>;
    void SetShellMenuCallback(ShellMenuCallback cb);
    // Returns a token identifying the session (0 when the manager is stopped).
    uint32_t QueryShellMenu(std::vector<std::wstring> paths, void* owner_hwnd,
                            bool background, bool extended,
                            std::vector<std::wstring> disabled_clsids = {});
    void InvokeShellMenu(uint32_t token, uint32_t item_id,
                         std::wstring verb = {}, std::wstring text = {}); // host auto-closes after
    void CloseShellMenu(uint32_t token);                    // dismissed without invoke
    // True if a context-menu InvokeCommand finished since the last take
    // (UI uses this to refresh the folder the verb may have mutated).
    bool TakeCtxInvokeDone();

    bool CanUndo() const;
    std::wstring UndoLabel() const;   // "撤销移动 xxx" etc; empty if none
    void Undo();

    OpStatus Status() const;
    std::vector<CompletedOperation> DrainCompletions();

    // Session persistence of the undo stack (JSON, same style as StagingTray).
    std::wstring UndoToJson() const;
    bool UndoFromJson(const std::wstring& in);

private:
    friend struct DeleteIntegrationProbe;
    friend struct TransferIntegrationProbe;
    friend class OperationElevationAdapter;
    friend struct ShellCompletionTestProbe;
    friend struct TerminalMenuTestProbe;
    friend struct UndoIdentityTestProbe;
    friend struct MutationUndoTestProbe;
    friend struct SourceContractTestProbe;
    friend struct OperationJournalProbe;
    struct QueueItem {
        OpRequest req;
        std::wstring open_path;   // non-empty => ShellExecuteEx instead
        std::wstring open_verb;   // "open" (default) / "properties" / ...
        std::wstring open_args;   // e.g. -d "<dir>" for wt.exe
        std::wstring open_file;   // explicit program (empty => open_path is the file)
        std::vector<std::wstring> open_paths; // multi-item "properties"
        uint64_t seq = 0;
        ULONGLONG enqueued_at = 0; // diagnostics: queue wait vs shell cost
        uint64_t recovery_sequence = 0; // retained until deletion is admitted
        bool lock_retry = false;
        bool elevated_retry = false;
        uint64_t retry_generation = 0;
        HRESULT retry_failure_hr = S_OK;
        std::wstring retry_failed_path;
        std::vector<std::wstring> retry_probe_paths; // ambiguous CopyFile2 phase: inspect both endpoints
        bool retry_rename_identity = false;
        FileIdentity rename_source_id;
        std::vector<LockOwner> close_first; // deliberately absent from persisted OpRequest
        std::vector<std::wstring> retry_completed_sources;
        std::vector<TransferPlanEntry> retry_transfer_plan;
        std::shared_ptr<DestinationGuard> retry_source_directories;
        std::shared_ptr<DestinationGuard> retry_destination_guard;
        std::vector<FileObjectReference> retry_source_references;
    };

    struct MenuJob {
        enum class Kind { Query, Invoke, Close } kind = Kind::Query;
        uint32_t token = 0;
        uint32_t item_id = 0;
        uint32_t owner_hwnd = 0;
        bool background = false;
        bool extended = false;
        std::vector<std::wstring> paths;
        std::vector<std::wstring> disabled_clsids;
        std::wstring verb;
        std::wstring text;
    };

    void WorkerThread();
    void StartShellClient();
    ipc::ShellClient::Owner ShellOwner() const;
    void MenuThread();
    struct OpenMailbox {
        ipc::ShellClient::Owner shell_owner;
        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable finished_cv;
        std::deque<QueueItem> queue;
        std::deque<std::wstring> errors;
        std::atomic<HWND> owner{nullptr};
        bool running = true;
        bool finished = false;
        std::function<void()> before_execute; // isolated regression gate, empty in production
    };
    static void OpenThread(std::shared_ptr<OpenMailbox> mailbox);
    // front = interactive dialog request (打开方式…, 属性): it jumps ahead of
    // queued opens so the dialog answers the click. Plain opens keep FIFO order.
    void EnqueueOpen(QueueItem item, bool front = false);
    void RunShellOp(const OpRequest& req, uint64_t task_id, const FileIdentity* rename_identity = nullptr,
                    bool lock_retry = false, const std::vector<FileIdentity>* continuation_identities = nullptr,
                    DestinationGuard* continuation_guard = nullptr);
    void RunManualRecycleRestore(const OpRequest& req,
        decltype(&IsCurrentUserRecyclePayload) owns_payload = &IsCurrentUserRecyclePayload,
        DestinationGuard* continuation_guard = nullptr);
    void RunDriveLabelRename(const OpRequest& req, uint64_t task_id,
                            decltype(&SetVolumeLabelW) set_label = &SetVolumeLabelW);
    void RunDelete(const QueueItem& item,
                   const std::vector<std::wstring>& prior_deleted = {},
                   const std::vector<std::wstring>& prior_recycled = {},
                   const std::vector<std::wstring>& prior_recycle_destinations = {});
    void FinishDelete(const QueueItem& item, std::wstring error,
                      const std::vector<std::wstring>& deleted_paths = {}, bool executed = false,
                      bool uncertain = false, const std::vector<std::wstring>& recycled_paths = {},
                      const std::vector<std::wstring>& recycle_destinations = {}, bool cancelled = false,
                      const LockReport& locks = {}, HRESULT failure_hr = S_OK,
                      const std::wstring& failed_path = {});
    DeleteService delete_service_;
    std::mutex delete_wait_mutex_;
    std::condition_variable delete_wait_cv_;
    std::atomic<bool> delete_active_{false};
    std::atomic<bool> delete_cancel_{false};
    struct ShellCompletion {
        uint32_t hr = 0;
        bool cancelled = false;
        std::wstring error;
        std::wstring failed_path;
        std::vector<std::wstring> deleted_paths;
        std::vector<std::wstring> recycled_paths;
        std::vector<std::wstring> recycle_destinations;
        std::vector<FileIdentity> produced_identities;
    };
    void OnShellDone(uint32_t id, ShellCompletion result);
    bool WaitShellDone(uint32_t id, ShellCompletion& result);
    void RunTransfer(const OpRequest& req, uint64_t task_id,
                     const std::vector<std::wstring>& prior_completed = {},
                     const std::vector<TransferPlanEntry>& prior_plan = {},
                     const std::shared_ptr<DestinationGuard>& prior_source_directories = {},
                     const std::shared_ptr<DestinationGuard>& prior_destination_guard = {});
    void ExecuteFileOperation(QueueItem& item);
    LockReport CaptureLockedFailure(const QueueItem& retry, HRESULT error,
                                   const std::wstring& failed_item, const LockCancelled& cancelled);
    static QueueItem RemainingDeleteRetry(const QueueItem& admitted, const DeletePlan& accepted,
                                         const std::vector<std::wstring>& actual);
    bool PrepareLockRetry(QueueItem& item);
    bool CaptureElevationFailure(QueueItem retry, HRESULT error, const std::wstring& failed_item);
    void SetStatus(const std::function<void(OpStatus&)>& fn);
    void RefreshElevationStatusLocked();
    void RefreshRecycleUndoValidity();
    void InvalidateRecycleUndo(const std::vector<std::wstring>& changed, bool uncertain = false);
    void PushUndo(const OpRequest& req,
                  const std::vector<std::wstring>* actual_destinations = nullptr,
                  const std::vector<FileIdentity>* produced_identities = nullptr);
    void FinishMutationUndo(const OpRequest& req);
    bool FinishMutationUndoLocked(const OpRequest& req); // caller holds mutex_
    void LoadRecoveryJournal();
    void PersistJournal();
    std::wstring JournalJsonLocked(const std::wstring& path) const;
    bool ConsumeCtxInvokeDone(uint32_t id);   // true = RSP_DONE was a menu invoke
    void RegisterShellMenuQuery(uint32_t token, uint32_t request_id);
    void RegisterShellMenuInvoke(uint32_t request_id);
    void OnCtxItems(uint32_t client_id, std::vector<ShellMenuItem> items, bool partial,
                    std::vector<std::wstring> slow_clsids);

    std::function<void()> notify_;

    mutable std::atomic<bool> undo_validation_requested_{false};
    uint64_t pending_recycle_undo_revision_ = 0; // mutex_; prevents duplicate validation requests
    mutable std::mutex mutex_;            // guards queue_ + status_ + undo_
    std::condition_variable cv_;
    std::deque<QueueItem> queue_;
    std::optional<QueueItem> active_item_;
    std::optional<QueueItem> lock_retry_;
    std::optional<QueueItem> elevation_retry_;
    bool elevation_worker_ = false;
    std::atomic<bool> lock_retry_active_{false};
    std::atomic<bool> lock_retry_cancel_{false};
    std::atomic<uint64_t> lock_retry_generation_{1};
    std::vector<RecoveryEntry> pending_recovery_;
    struct JournalState;
    // Shell operation workers do not configure application recovery journals.
    std::unique_ptr<JournalState> journal_;
    OpStatus status_;
    std::deque<UndoEntry> undo_;
    uint64_t undo_revision_ = 1;
    std::deque<CompletedOperation> completions_;
    std::deque<DeleteOutcome> delete_outcomes_;
    std::set<uint64_t> scheduled_recovery_;

    std::thread thread_;
    bool running_ = false;
    bool accepting_ = true; // mutex_; shutdown rejects submissions until Start
    bool file_operation_in_flight_ = false; // covers dequeue before status publication
    uint64_t pending_mutation_undo_revision_ = 0; // mutex_; one queued or executing mutation inverse
    std::atomic<bool> stopping_{false};
    uint64_t next_seq_ = 1;

    std::atomic<uint32_t> current_req_id_{0};
    std::atomic<bool> shell_cancel_requested_{false};
    std::atomic<ULONGLONG> shell_activity_tick_{0};
    std::atomic<bool> transfer_active_{false};
    std::atomic<bool> transfer_cancel_{false};
    std::atomic<bool> transfer_pause_{false};
    std::atomic<bool> verify_copies_{false};
    decltype(&GetFileInformationByHandleEx) transfer_identity_query_ = GetFileInformationByHandleEx;
    decltype(&CopyFile2) transfer_copy_file_ = CopyFile2;
    std::function<void(const std::wstring&)> before_source_cleanup_; // regression gate, empty in production
    std::function<void(const std::wstring&)> before_source_enumeration_; // regression gate, empty in production
    bool force_copy_move_ = false; // isolated cross-volume regression without a second user volume

    mutable std::mutex transfer_control_mutex_;
    std::condition_variable transfer_control_cv_;
    std::optional<ConflictItemInfo> pending_conflict_;
    uint64_t next_conflict_token_ = 1;
    uint64_t resolved_conflict_token_ = 0;
    ConflictChoice resolved_conflict_choice_ = ConflictChoice::Cancel;
    bool resolved_conflict_apply_all_ = false;

    // Each request owns its terminal result, including deletion evidence.
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    std::map<uint32_t, ShellCompletion> done_results_;

    // Context-menu forwarding thread + token <-> pipe-request-id bookkeeping.
    std::thread menu_thread_;
    bool menu_running_ = false;
    mutable std::mutex menu_mutex_;
    std::condition_variable menu_cv_;
    std::deque<MenuJob> menu_queue_;
    ShellMenuCallback menu_cb_;
    uint32_t next_menu_token_ = 1;
    std::map<uint32_t, uint32_t> menu_session_by_token_;  // token -> query req id
    std::map<uint32_t, uint32_t> menu_token_by_session_;  // query req id -> token
    std::set<uint32_t> ctx_invoke_ids_;                   // in-flight invoke req ids
    std::atomic<uint32_t> ctx_invoke_done_{0};

    // Dedicated IPC forwarding thread; Shell code runs outside this process.
    std::thread open_thread_;
    // Owner for the shell dialogs above; written by the UI thread only.
    std::atomic<HWND> ui_hwnd_{nullptr};
    std::shared_ptr<OpenMailbox> open_mailbox_;
    mutable std::mutex open_mutex_;
};

// wt.exe argument string for "open terminal here" (unit-tested; launching is
// compile-verified only in 1B-2).
std::wstring TerminalCommandLine(const std::wstring& dir);

} // namespace pulse::ops
