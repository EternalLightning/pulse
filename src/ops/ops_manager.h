// ops_manager.h — Operation queue, progress status, undo stack (UI-process side).
//
// All file operations are serialized on one worker thread and executed through
// Pulse.Shell.exe (ShellClient). The UI thread never touches COM/shell:
// it calls Submit/Cancel/Undo/Status and gets repaints via the notify callback.
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
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops {

struct DeleteIntegrationProbe;

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
    std::vector<std::wstring> recycle_paths; // exact recycle payloads for Undo restore
    std::vector<RecycleUndoIdentity> recycle_undo_identities; // in-session worker proof, never persisted
};

// Worker snapshot used only for an in-session retry; never journaled. Keeping
// exact descendants and destinations avoids rescanning unrelated new files.
struct TransferPlanEntry {
    std::wstring source;
    std::wstring destination;
    bool directory = false;
    bool reparse = false;
    bool destination_preexisting = false;
    uint64_t bytes = 0;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME modified{};
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
    bool lock_retry_available = false; // single rename can retry without Restart Manager owners
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
};

struct RecoveryEntry {
    uint64_t sequence = 0;
    OpRequest request;
    bool was_active = false;
};

struct RecoverySnapshot {
    std::vector<RecoveryEntry> entries;
    bool has_uncertain_destructive = false;
};

// One Explorer verb coming back from a pulse_shell context-menu session.
// Software submenus keep one level: header row (has_children) + child rows.
struct ShellMenuItem {
    uint32_t id = 0;             // host menu id; pass to InvokeShellMenu
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;
    bool child = false;
    std::wstring verb;           // canonical verb (may be empty)
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
};

class OpsManager {
public:
    OpsManager() = default;
    ~OpsManager();

    // notify is invoked from worker/IPC threads whenever status changes.
    void Start(std::function<void()> notify);
    void Stop();
    bool HasPendingFileOperations() const;
    void BeginShutdown();

    void SetJournalPath(std::wstring path);
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
    std::optional<DeleteConfirmation> PendingDeleteConfirmation() const;
    void ResolveDeleteConfirmation(uint64_t token, bool accepted);
    std::vector<DeleteOutcome> DrainDeleteOutcomes();

    // Double-click open: ShellExecuteEx on a dedicated open thread (plan §6.2).
    void OpenWith(const std::wstring& path);

    // Shell "properties" verb on the ops worker thread. Compile-verified only
    // in 1B-2; wired to the context menu but exercised manually.
    void ShowProperties(const std::wstring& path);
    // Several items: one merged Explorer sheet (SHMultiFileProperties) on a
    // short-lived STA thread that pumps messages while the sheet is open.
    void ShowProperties(const std::vector<std::wstring>& paths);

    // Any registry shell verb ("print", "edit", …) via ShellExecuteEx on the
    // ops worker thread. "openas" / "打开方式…" uses SHOpenWithDialog.
    void ExecuteVerb(const std::wstring& path, const std::wstring& verb);

    // Open `file` with a specific application (open-with MRU entry).
    void OpenWithApp(const std::wstring& app_exe, const std::wstring& file);

    // Expand a registry command template (%1/%L) and launch it on the open thread.
    void ExecuteCommand(const std::wstring& command, const std::wstring& path);

    // `wt.exe -d <dir>` on the ops worker thread. Compile-verified only.
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
        uint64_t retry_generation = 0;
        HRESULT retry_failure_hr = S_OK;
        std::wstring retry_failed_path;
        std::vector<std::wstring> retry_probe_paths; // ambiguous CopyFile2 phase: inspect both endpoints
        bool retry_rename_identity = false;
        FILE_ID_INFO rename_source_id{};
        std::vector<LockOwner> close_first; // deliberately absent from persisted OpRequest
        std::vector<std::wstring> retry_completed_sources;
        std::vector<TransferPlanEntry> retry_transfer_plan;
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
    void MenuThread();
    struct OpenMailbox {
        std::mutex mutex;
        std::condition_variable cv;
        std::condition_variable finished_cv;
        std::deque<QueueItem> queue;
        std::atomic<HWND> owner{nullptr};
        bool running = true;
        bool finished = false;
        std::function<void()> before_execute; // isolated regression gate, empty in production
    };
    static void OpenThread(std::shared_ptr<OpenMailbox> mailbox);
    // front = interactive dialog request (打开方式…, 属性): it jumps ahead of
    // queued opens so the dialog answers the click. Plain opens keep FIFO order.
    void EnqueueOpen(QueueItem item, bool front = false);
    void RunShellOp(const OpRequest& req, uint64_t task_id, const FILE_ID_INFO* rename_identity = nullptr,
                    bool lock_retry = false);
    void RunDelete(const QueueItem& item,
                   const std::vector<std::wstring>& prior_deleted = {},
                   const std::vector<std::wstring>& prior_recycled = {},
                   const std::vector<std::wstring>& prior_recycle_destinations = {});
    void FinishDelete(const QueueItem& item, std::wstring error,
                      const std::vector<std::wstring>& deleted_paths = {}, bool executed = false,
                      bool uncertain = false, const std::vector<std::wstring>& recycled_paths = {},
                      const std::vector<std::wstring>& recycle_destinations = {}, bool cancelled = false,
                      const LockReport& locks = {});
    DeleteService delete_service_;
    std::mutex delete_wait_mutex_;
    std::condition_variable delete_wait_cv_;
    std::atomic<bool> delete_active_{false};
    std::atomic<bool> delete_cancel_{false};
    bool WaitShellDone(uint32_t id, uint32_t& hr, bool& cancelled, std::wstring& error);
    void RunTransfer(const OpRequest& req, uint64_t task_id,
                     const std::vector<std::wstring>& prior_completed = {},
                     const std::vector<TransferPlanEntry>& prior_plan = {});
    void ExecuteFileOperation(QueueItem& item);
    LockReport CaptureLockedFailure(const QueueItem& retry, HRESULT error,
                                   const std::wstring& failed_item, const LockCancelled& cancelled);
    static QueueItem RemainingDeleteRetry(const QueueItem& admitted, const DeletePlan& accepted,
                                         const std::vector<std::wstring>& actual);
    bool PrepareLockRetry(QueueItem& item);
    void SetStatus(const std::function<void(OpStatus&)>& fn);
    void RefreshRecycleUndoValidity();
    void InvalidateRecycleUndo(const std::vector<std::wstring>& changed, bool uncertain = false);
    void PushUndo(const OpRequest& req,
                  const std::vector<std::wstring>* actual_destinations = nullptr);
    void LoadRecoveryJournal();
    void PersistJournal();
    std::wstring JournalJsonLocked() const;
    bool ConsumeCtxInvokeDone(uint32_t id);   // true = RSP_DONE was a menu invoke
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
    std::atomic<bool> lock_retry_active_{false};
    std::atomic<bool> lock_retry_cancel_{false};
    std::atomic<uint64_t> lock_retry_generation_{1};
    std::vector<RecoveryEntry> pending_recovery_;
    std::wstring journal_path_;
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
    std::set<uint64_t> move_undo_reservations_;
    std::atomic<bool> stopping_{false};
    uint64_t next_seq_ = 1;

    std::atomic<uint32_t> current_req_id_{0};
    std::atomic<bool> shell_cancel_requested_{false};
    std::atomic<ULONGLONG> shell_activity_tick_{0};
    std::atomic<bool> transfer_active_{false};
    std::atomic<bool> transfer_cancel_{false};
    std::atomic<bool> transfer_pause_{false};
    std::atomic<bool> verify_copies_{false};

    mutable std::mutex transfer_control_mutex_;
    std::condition_variable transfer_control_cv_;
    std::optional<ConflictItemInfo> pending_conflict_;
    uint64_t next_conflict_token_ = 1;
    uint64_t resolved_conflict_token_ = 0;
    ConflictChoice resolved_conflict_choice_ = ConflictChoice::Cancel;
    bool resolved_conflict_apply_all_ = false;

    // Completion sync for the op currently in flight.
    std::mutex done_mutex_;
    std::condition_variable done_cv_;
    bool done_ready_ = false;
    uint32_t done_id_ = 0;
    uint32_t done_hr_ = 0;
    bool done_cancelled_ = false;
    std::wstring done_error_;
    std::vector<std::wstring> done_deleted_paths_;
    std::vector<std::wstring> done_recycled_paths_;
    std::vector<std::wstring> done_recycle_destinations_;

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

    // ShellExecute / properties: dedicated STA thread so opens never
    // serialize behind transfers (SEE_MASK_NOASYNC on the transfer
    // worker made double-click open wait for in-flight copies).
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
