#include "../app/app_internal.h"
#include "../ops/delete_operation.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ipc/ctx_menu_util.h"
#include "../ipc/delete_plan_protocol.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>

LRESULT CALLBACK WndProcImpl(HWND, UINT, WPARAM, LPARAM);

namespace pulse::ops {
struct DeleteIntegrationProbe {
    static void Recover(OpsManager& manager, std::vector<RecoveryEntry> entries) {
        manager.pending_recovery_ = std::move(entries);
    }
    static size_t Queued(const OpsManager& manager) {
        return manager.queue_.size();
    }
    static void RunQueued(OpsManager& manager) {
        OpsManager::QueueItem item;
        {
            std::lock_guard<std::mutex> lock(manager.mutex_);
            if (manager.queue_.empty()) throw std::runtime_error("Expected one queued deletion fixture request");
            item = std::move(manager.queue_.front()); manager.queue_.pop_front();
        }
        manager.RunDelete(item);
    }
    static void Notify(OpsManager& manager, std::function<void()> notify) {
        manager.notify_ = std::move(notify);
    }
    static std::wstring Journal(const OpsManager& manager) { return manager.JournalJsonLocked(); }
    static void UncertainResult(OpsManager& manager, OpRequest request) {
        OpsManager::QueueItem item;
        item.seq = manager.next_seq_++; item.req = std::move(request);
        manager.active_item_ = item;
        manager.FinishDelete(item, L"mock lost backend result", {}, true, true);
    }
    static void FailedResult(OpsManager& manager, OpRequest request) {
        OpsManager::QueueItem item;
        item.seq = manager.next_seq_++; item.req = std::move(request);
        manager.FinishDelete(item, L"fixture access denied");
    }
    static void UncertainUndo(OpsManager& manager, OpRequest request) {
        request.is_undo = true; request.undo_revision = manager.undo_revision_;
        UncertainResult(manager, std::move(request));
    }
    static void StopFlag(OpsManager& manager, bool stopped) { manager.stopping_.store(stopped); }
    static uint64_t Request(OpsManager& manager, OpRequest request) {
        OpsManager::QueueItem item;
        item.seq = manager.next_seq_++; item.req = std::move(request);
        manager.RunDelete(item);
        return item.seq;
    }
};
}

namespace {
int dialog_key = VK_ESCAPE;
bool saw_dialog = false;
std::wstring network_capture;
bool network_capture_ok = false;
bool CaptureConfirmation(HWND window) {
#ifdef PULSE_UI_TEST_HOOKS
    return SendMessageW(window, pulse::ui::kConfirmSnapshotMessage, 0,
        reinterpret_cast<LPARAM>(network_capture.c_str())) != 0;
#else
    (void)window;
    return false;
#endif
}
void CALLBACK RejectDialog(HWND owner, UINT, UINT_PTR id, DWORD) {
    HWND dialog = FindWindowW(L"PulseConfirmWindow", nullptr);
    if (!dialog) return;
    saw_dialog = true;
    KillTimer(owner, id);
    if (!network_capture.empty()) {
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        network_capture_ok = CaptureConfirmation(dialog);
    }
    if (dialog_key == WM_CLOSE) PostMessageW(dialog, WM_CLOSE, 0, 0);
    else PostMessageW(dialog, WM_KEYDOWN, dialog_key, 0);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !passed;
    };
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Delete fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 1280, 900, nullptr, nullptr, nullptr, nullptr);
    check(owner != nullptr, "isolated owner window exists");
    if (argc == 2 && std::string_view(argv[1]) == "--network-confirmation") {
        using Probe = ops::DeleteIntegrationProbe;
        auto state = std::make_unique<AppState>(); auto& s = *state;
        s.isolatedTest = true; s.appPrefs.persist = false; s.searchHistory.persist = false;
        s.hwnd = owner; s.ops.SetUiWindow(owner);
        s.operationWindow = std::make_unique<ui::FileOperationWindow>();
        check(s.operationWindow->Create(owner, {}), "create isolated Pulse progress window");
        std::filesystem::create_directories(L"bench_data/network-confirmation");
        for (const auto locale : {L"en-US", L"zh-CN"}) {
            l10n::SetLanguage(locale);
            s.darkMode = std::wstring_view(locale) == L"zh-CN";
            s.operationWindow->SetTheme(s.darkMode, s.accentColor);
            s.operationWindow->Show(false);
            Probe::Notify(s.ops, [&] {
                const auto pending = s.ops.PendingDeleteConfirmation();
                if (!pending) return;
                const auto spec = BuildDeleteConfirmationSpec(*pending);
                const auto& reason = l10n::Get(l10n::StringId::DeleteReasonNetwork);
                check(!reason.empty() && spec.message.find(reason) != std::wstring::npos &&
                    spec.cancel_is_default && spec.danger,
                    "Pulse confirmation includes localized network reason and defaults to cancellation");
                wchar_t count[256]{};
                swprintf_s(count, l10n::Get(l10n::StringId::DeleteCountFormat).c_str(), 2ull, 1ull, 1ull);
                check(spec.message.find(count) != std::wstring::npos, "mixed confirmation counts one permanent and one recycle request");
                network_capture = std::wstring(L"bench_data/network-confirmation/") + locale + L".png";
                saw_dialog = false; network_capture_ok = false; dialog_key = VK_RETURN;
                SetTimer(owner, 91, 300, RejectDialog);
                PresentDeleteConfirmation(s);
                KillTimer(owner, 91);
                check(saw_dialog && network_capture_ok && !s.operationWindow->IsVisible(),
                    "actual Pulse confirmation renders while previous progress window is hidden");
                network_capture.clear();
            });
            ops::OpRequest request; request.type = ops::OpType::RecycleDelete;
            request.sources = {L"C:\\Pulse-isolated-fixture\\local.txt", L"Z:\\Pulse-read-only-plan-check.txt"};
            Probe::Request(s.ops, request);
            check(s.ops.Status().phase == ops::OpPhase::Cancelled && s.ops.DrainCompletions().empty(),
                "default Enter rejects network deletion before any backend invocation");
        }
        Probe::Notify(s.ops, {}); s.operationWindow->Destroy(); s.hwnd = nullptr;
        DestroyWindow(owner); OleUninitialize(); return failures ? 1 : 0;
    }
    check(ipc::IsLosslessDeleteShellPath(L"\\\\?\\C:\\Fixture\\normal.txt") &&
        ipc::IsLosslessDeleteShellPath(L"\\\\?\\UNC\\server\\share\\normal.txt") &&
        !ipc::IsLosslessDeleteShellPath(L"\\\\?\\C:\\Fixture\\name.") &&
        !ipc::IsLosslessDeleteShellPath(L"\\\\?\\C:\\Fixture\\name ") &&
        !ipc::IsLosslessDeleteShellPath(L"\\\\?\\C:\\Fixture\\NUL.txt"),
        "lossless Shell guard accepts normal extended/UNC and rejects ambiguous trailing/device names");
    ipc::PayloadWriter wire;
    wire.PutU64(123); wire.PutU64(456); wire.PutStringArray({L"C:\\Fixture\\Foo", L"C:\\Fixture\\foo"});
    ipc::PayloadReader reader(wire.data().data(), wire.data().size());
    ipc::DeleteWirePlan parsed;
    ipc::DeleteHostAdmission gate;
    check(ipc::ReadDeleteWirePlan(reader, parsed) && !gate.Consume(parsed, 124) &&
        gate.Consume(parsed, 123) && !gate.Consume(parsed, 123) && parsed.paths.size() == 2,
        "host exact-plan parsing rejects stale epoch and repeated token without losing case-distinct roots");
    ops::DeletePlan case_plan;
    case_plan.targets = {{L"C:\\Fixture\\Foo", {L"C:\\Fixture\\Foo"}, ops::DeleteDisposition::Permanent, L""},
        {L"C:\\Fixture\\foo", {L"C:\\Fixture\\foo"}, ops::DeleteDisposition::Permanent, L""}};
    check(ops::MapDeletedTargets(case_plan, {L"C:\\Fixture\\Foo"}) == std::vector<std::wstring>{L"C:\\Fixture\\Foo"},
        "case-distinct physical results never invent deletion of the other logical item");
    ops::DeletePlan pair_plan;
    pair_plan.targets = {{L"C:\\original\\one.txt", {L"C:\\$Recycle.Bin\\S-1-5-21-test\\$Rone", L"C:\\$Recycle.Bin\\S-1-5-21-test\\$Ione"},
        ops::DeleteDisposition::Permanent, L""}};
    check(!ops::IsDeleteSnapshotComplete(pair_plan, {pair_plan.targets[0].physical_paths[0]}) &&
        ops::IsDeleteSnapshotComplete(pair_plan, pair_plan.targets[0].physical_paths) &&
        ops::MapDeletedTargets(pair_plan, {pair_plan.targets[0].physical_paths[0]}).size() == 1,
        "$R completion counts one logical item but cannot hide unconfirmed $I cleanup");
    {
        ui::ConfirmDialogSpec spec;
        spec.title = L"Isolated permanent delete test"; spec.message = L"Cannot be undone. 2 items. C:\\Fixture\\one.txt";
        spec.confirm_text = L"Permanently delete"; spec.cancel_text = L"Cancel";
        spec.danger = true; spec.cancel_is_default = true; spec.fit_to_work_area = true;
        for (int key : {VK_RETURN, VK_SPACE, VK_ESCAPE, WM_CLOSE}) {
            saw_dialog = false; dialog_key = key;
            SetTimer(owner, 91, 10, RejectDialog);
            const bool accepted = ui::ShowConfirmDialog(owner, spec, false, ui::HexColor(0x0078D4));
            KillTimer(owner, 91);
            check(saw_dialog && !accepted, "native confirm default Enter/Space/Esc/close rejects");
        }
        ops::DeleteConfirmation mixed;
        mixed.token = 12;
        mixed.plan.targets = {{L"C:\\Fixture\\keep-in-bin.txt", {L"C:\\Fixture\\keep-in-bin.txt"}, ops::DeleteDisposition::Recyclable, L"mock proven recycle"},
            {L"C:\\Fixture\\permanent.txt", {L"C:\\Fixture\\permanent.txt"}, ops::DeleteDisposition::Permanent, L"explicit permanent"}};
        auto actual_spec = BuildDeleteConfirmationSpec(mixed);
        check(actual_spec.cancel_is_default && actual_spec.danger && actual_spec.fit_to_work_area &&
            actual_spec.message.find(L"2 items (1 permanent, 1 recycled)") != std::wstring::npos &&
            actual_spec.message.find(l10n::Get(l10n::StringId::DeleteMixedWarning)) != std::wstring::npos &&
            actual_spec.message.find(L"explicit permanent") != std::wstring::npos,
            "shared confirmation explicitly summarizes irreversible reason and mixed logical count");
        for (int i = 0; i < 20; ++i) mixed.plan.targets.push_back({std::wstring(4000, L'x'), {L"C:\\Fixture\\long.txt"}, ops::DeleteDisposition::Permanent, L"explicit permanent"});
        actual_spec = BuildDeleteConfirmationSpec(mixed);
        check(actual_spec.message.size() < 1600 && actual_spec.message.find(L"more items") != std::wstring::npos,
            "large/long-path batch summary is bounded without losing total logical count");
        saw_dialog = false; dialog_key = VK_RETURN;
        SetTimer(owner, 91, 10, RejectDialog);
        check(!ui::ShowConfirmDialog(owner, actual_spec, true, ui::HexColor(0x0078D4)) && saw_dialog,
            "actual long-batch confirmation retains native default cancellation");
        KillTimer(owner, 91);
        spec.still_valid = [] { return false; };
        check(!ui::ShowConfirmDialog(owner, spec, true, ui::HexColor(0x0078D4)), "expired modal confirmation rejects without presenter input");
    }
    auto state = std::make_unique<AppState>(); auto& s = *state;
    s.isolatedTest = true; s.appPrefs.persist = false; s.searchHistory.persist = false;
    s.hwnd = owner; SetWindowLongPtrW(owner, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = L"C:\\Fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const wchar_t* name : {L"first.txt", L"second.txt", L"third.txt"}) {
        fs::DirEntry entry; entry.name = name; entry.attrs = FILE_ATTRIBUTE_NORMAL; entries->push_back(entry);
    }
    tab.SetSnapshot(entries); tab.SelectOnly(1); tab.scroll_y = 30;
    s.ops.SetUiWindow(owner);
    using Probe = ops::DeleteIntegrationProbe;
    s.operationWindow = std::make_unique<ui::FileOperationWindow>();
    check(s.operationWindow->Create(owner, {}), "isolated operation window exists for cancellation presentation regression");
    const auto unchanged = [&] {
        return tab.selected_index == 1 && tab.SelectedCount() == 1 && tab.IsSelected(1) && tab.scroll_y == 30;
    };
    auto revision = tab.selection_revision;
    const auto finished = [&] {
        Probe::RunQueued(s.ops);
        WndProcImpl(owner, WM_OPS_NOTIFY, 0, 0);
        check(unchanged() && tab.selection_revision == revision, "refusal WM_OPS_NOTIFY does not refresh or change selection");
        check(s.ops.DrainCompletions().empty(), "refused deletion emits no successful mutation completion");
    };
    ops::OpRequest ordinary;
    ordinary.type = ops::OpType::RecycleDelete; ordinary.sources = {L"C:\\Fixture\\first.txt"};
    std::wstring ordinary_error;
    const auto ordinary_plan = ops::BuildDeletePlan(ordinary, 1, ordinary_error);
    ops::DeleteService ordinary_gate;
    check(ordinary_error.empty() && ordinary_plan.targets.size() == 1 &&
        ordinary_plan.targets[0].disposition == ops::DeleteDisposition::RecycleRequested &&
        ordinary_gate.Prepare(ordinary_plan).decision == ops::DeleteDecision::Admitted,
        "ordinary Delete admits recycling intent to Shell instead of requiring impossible preflight proof");
    DeleteSelected(s, false);
    check(Probe::Queued(s.ops) == 1, "normal Delete routes to operations gate");
    finished();
    check(!s.ops.PendingDeleteConfirmation() && s.ops.Status().deletes_without_mutation == 1,
        "isolated recycle request with stopped backend finishes without mutation or Pulse confirmation");
    ops::OpRequest permanent;
    permanent.type = ops::OpType::RealDelete;
    permanent.sources = {L"C:\\Fixture\\first.txt", L"\\\\server\\share\\中文.txt"};
    uint64_t confirmation_token = 0;
    Probe::Notify(s.ops, [&] {
        if (const auto pending = s.ops.PendingDeleteConfirmation()) {
            confirmation_token = pending->token;
            s.ops.ResolveDeleteConfirmation(pending->token + 1, true);
            s.ops.ResolveDeleteConfirmation(pending->token, false);
        }
    });
    const auto before = s.ops.Status().completed_ops;
    Probe::Request(s.ops, permanent);
    check(confirmation_token != 0 && s.ops.Status().completed_ops == before + 1,
        "permanent Submit requires exact-token rejection and finishes once");
    check(s.ops.Status().deletes_without_mutation == 2, "cancelled permanent request is proven zero-mutation");
    WndProcImpl(owner, WM_OPS_NOTIFY, 0, 0);
    check(unchanged() && tab.selection_revision == revision, "permanent cancel preserves selection and scroll");
    check(s.ops.Status().phase == ops::OpPhase::Cancelled && s.ops.Status().last_error.empty() &&
        !s.operationWindow->IsVisible() && !s.notification_toast.IsVisible(),
        "permanent confirmation rejection is silent cancellation, never a failure popup");
    Probe::Notify(s.ops, [&] {
        if (!s.ops.PendingDeleteConfirmation()) return;
        saw_dialog = false; dialog_key = VK_RETURN;
        SetTimer(owner, 91, 10, RejectDialog);
        PresentDeleteConfirmation(s);
        KillTimer(owner, 91);
    });
    Probe::Request(s.ops, permanent);
    check(saw_dialog && !s.ops.PendingDeleteConfirmation() && s.ops.DrainCompletions().empty(),
        "actual shared presenter rejects permanent deletion on default Enter");
    WndProcImpl(owner, WM_OPS_NOTIFY, 0, 0);
    check(!s.operationWindow->IsVisible(), "default Enter cancellation does not show the operation failure window");
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (int key : {VK_ESCAPE, WM_CLOSE}) {
            Probe::Notify(s.ops, [&] {
                if (!s.ops.PendingDeleteConfirmation()) return;
                saw_dialog = false; dialog_key = key;
                SetTimer(owner, 91, 10, RejectDialog);
                PresentDeleteConfirmation(s);
                KillTimer(owner, 91);
            });
            Probe::Request(s.ops, permanent);
            WndProcImpl(owner, WM_OPS_NOTIFY, 0, 0);
            check(saw_dialog && s.ops.Status().phase == ops::OpPhase::Cancelled &&
                s.ops.Status().last_error.empty() && !s.operationWindow->IsVisible() &&
                !s.notification_toast.IsVisible(),
                "Escape and close permanent cancellation stay silent in both display languages");
        }
    }
    l10n::SetLanguage(L"en-US");
    s.operationWindow->Show(false);
    s.operationPinnedByUser = true;
    UpdateOperationWindow(s, false);
    check(!s.operationWindow->IsVisible(), "cancelled task hides an already visible pinned operation window");
    s.operationPinnedByUser = false;
    Probe::FailedResult(s.ops, permanent);
    UpdateOperationWindow(s, false);
    check(s.ops.Status().phase == ops::OpPhase::Failed && s.operationWindow->IsVisible(),
        "genuine permanent deletion failure still shows its error window");
    s.operationWindow->Hide();
    WndProcImpl(owner, WM_OPS_NOTIFY, 0, 0);
    s.operationWindow->Hide();
    Probe::Notify(s.ops, {});
    Probe::Request(s.ops, permanent);
    check(!s.ops.PendingDeleteConfirmation(), "missing presenter is fail closed");
    Probe::Notify(s.ops, [&] { if (s.ops.PendingDeleteConfirmation()) s.ops.CancelCurrent(); });
    Probe::Request(s.ops, permanent);
    check(!s.ops.PendingDeleteConfirmation(), "CancelCurrent while pending rejects without backend execution");
    Probe::Notify(s.ops, [&] {
        if (const auto pending = s.ops.PendingDeleteConfirmation()) s.ops.ResolveDeleteConfirmation(pending->token, true);
        else if (s.ops.Status().phase == ops::OpPhase::Running) s.ops.CancelCurrent();
    });
    const auto cancelled_before_send = s.ops.Status().deletes_without_mutation;
    Probe::Request(s.ops, permanent);
    check(s.ops.Status().deletes_without_mutation == cancelled_before_send + 1 &&
        s.ops.PendingRecovery().entries.empty(), "Cancel in Running notify closes the pre-send handoff without uncertain execution");
    Probe::Notify(s.ops, {});
    const std::wstring undo_json = LR"([{"type":0,"sup":true,"dest":"C:\\Fixture","name":"","src":["C:\\original\\first.txt"],"dst":["C:\\Fixture\\first.txt"]}])";
    check(s.ops.UndoFromJson(undo_json), "isolated Undo stack imports");
    const auto original_undo = s.ops.UndoToJson();
    s.ops.Undo();
    check(s.ops.UndoToJson() == original_undo && Probe::Queued(s.ops) == 1, "Undo copy peeks rather than pops before deletion gate");
    revision = tab.selection_revision;
    finished();
    check(s.ops.UndoToJson() == original_undo, "unexecuted Undo deletion retains the same Undo entry");
    ops::RecoveryEntry recovery;
    recovery.sequence = 77; recovery.request.type = ops::OpType::RecycleDelete;
    recovery.request.sources = {L"C:\\Fixture\\first.txt"};
    Probe::Recover(s.ops, {recovery});
    s.ops.RetryRecovery(); finished();
    check(s.ops.PendingRecovery().entries.size() == 1, "unadmitted Recovery deletion retains its journal entry");
    recovery.sequence = 78; recovery.request.type = ops::OpType::Copy; recovery.request.dest_dir = L"C:\\Fixture";
    Probe::Recover(s.ops, {recovery});
    check(!s.ops.RetryRecovery() && Probe::Queued(s.ops) == 0 && s.ops.PendingRecovery().entries.size() == 1,
        "Recovery cannot trial-delete temporary-name matches");
    s.ops.DiscardRecovery();
    check(s.ops.PendingRecovery().entries.empty(), "DiscardRecovery forgets requests without deleting temporary-name files");
    s.duplicateScan.groups = {{1, 1024, {{L"C:\\Fixture\\first.txt", L"first.txt", 1024, 0},
        {L"C:\\Fixture\\second.txt", L"second.txt", 1024, 0}}, 0}};
    RecycleDuplicateGroup(s, 0); finished();
    RecycleAllDuplicateExtras(s); finished();
    check(s.duplicateScan.groups.size() == 1 && s.duplicateScan.groups[0].keep_index == 0 &&
        s.duplicateScan.groups[0].files.size() == 2, "duplicate group/all refusal preserves keeper and results");
    const bool preview_ready = s.quickPreview.Initialize(owner, WM_QUICK_PREVIEW_NAVIGATE, WM_QUICK_PREVIEW_OPEN, WM_QUICK_PREVIEW_COMMAND);
    check(preview_ready, "isolated preview window initializes");
    if (!preview_ready) {
        std::cout << "[FAIL] preview D3D initialization precondition; stop GUI-dependent tests" << std::endl;
        SetWindowLongPtrW(owner, GWLP_USERDATA, 0); s.hwnd = nullptr;
        DestroyWindow(owner); OleUninitialize();
        return 2;
    }
    ui::QuickPreviewItem shown;
    shown.name = L"first.txt"; shown.path = EntryFullPath(tab, 0);
    shown.attrs = FILE_ATTRIBUTE_OFFLINE; // Never load a real provider or media backend.
    s.quickPreview.Show(shown, false, ui::WindowEffect::None, true);
    Pump();
    revision = tab.selection_revision;
    HandleQuickPreviewCommand(s, ui::QuickPreviewAction::Delete, false);
    check(unchanged() && tab.selection_revision == revision && s.quickPreview.item().path == shown.path,
        "preview Delete branches before SelectOnly and preserves unrelated selection");
    finished();
    check(s.quickPreview.visible() && s.quickPreview.item().path == shown.path && !s.previewDeleteIntent,
        "preview refusal clears only intent, not preview or list state");
    Probe::Notify(s.ops, [&] {
        if (const auto pending = s.ops.PendingDeleteConfirmation()) s.ops.ResolveDeleteConfirmation(pending->token, false);
    });
    revision = tab.selection_revision;
    HandleQuickPreviewCommand(s, ui::QuickPreviewAction::Delete, true);
    finished();
    check(s.quickPreview.visible() && s.quickPreview.item().path == shown.path && tab.selection_revision == revision,
        "preview permanent cancellation preserves preview and unrelated selection");
    Probe::Notify(s.ops, {});
    // Exercise watcher-before-DONE and DONE-before-watcher without a real delete.
    auto remaining = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    remaining->erase(remaining->begin());
    s.previewDeleteIntent = AppState::PreviewDeleteIntent{9001, &tab, tab.view_generation,
        tab.current_path, shown.path, 0, false};
    tab.SetSnapshot(remaining);
    SyncQuickPreview(s);
    check(s.quickPreview.visible() && s.quickPreview.item().path == shown.path && s.previewDeleteIntent,
        "watcher-before-DONE waits without consuming preview intent");
    s.previewDeleteIntent->succeeded = true;
    SyncQuickPreview(s);
    check(s.quickPreview.visible() && s.quickPreview.item().name == L"second.txt" && !s.previewDeleteIntent,
        "matching successful DONE after watcher selects the next preview item");
    tab.SetSnapshot(entries);
    s.quickPreview.Update(shown);
    s.previewDeleteIntent = AppState::PreviewDeleteIntent{9002, &tab, tab.view_generation,
        tab.current_path, shown.path, 0, true};
    SyncQuickPreview(s);
    check(s.previewDeleteIntent && s.quickPreview.item().path == shown.path, "DONE-before-watcher retains intent while source is still listed");
    tab.SetSnapshot(remaining); SyncQuickPreview(s);
    check(s.quickPreview.item().name == L"second.txt" && !s.previewDeleteIntent, "watcher after DONE consumes the exact successful preview intent");
    tab.SetSnapshot(entries); s.quickPreview.Update(shown);
    s.previewDeleteIntent = AppState::PreviewDeleteIntent{9003, &tab, tab.view_generation,
        tab.current_path, shown.path, 0, true};
    ++tab.view_generation;
    SyncQuickPreview(s);
    check(!s.previewDeleteIntent, "navigation generation invalidates stale preview deletion intent");
    check(ipc::IsBuiltinContextVerb(L"delete", false) && !ipc::IsBuiltinContextVerb(L"third-party-secure-erase", false),
        "native delete is filtered and unknown third-party Shell verb stays outside Pulse boundary");
    // Pending deletion must not enter the execution journal.
    bool pending_not_journaled = false;
    Probe::Notify(s.ops, [&] {
        if (const auto pending = s.ops.PendingDeleteConfirmation()) {
            pending_not_journaled = Probe::Journal(s.ops).find(L"first.txt") == std::wstring::npos;
            s.ops.ResolveDeleteConfirmation(pending->token, false);
        }
    });
    Probe::Request(s.ops, permanent);
    check(pending_not_journaled, "pending permanent confirmation is absent from execution journal");
    Probe::StopFlag(s.ops, true);
    Probe::Request(s.ops, permanent);
    check(!s.ops.PendingDeleteConfirmation(), "stop before deletion preparation is fail closed");
    recovery.sequence = 79; recovery.was_active = true; recovery.request = permanent;
    Probe::Recover(s.ops, {recovery});
    check(!s.ops.RetryRecovery() && Probe::Queued(s.ops) == 0 && s.ops.PendingRecovery().has_uncertain_destructive,
        "uncertain attempted deletion is retained for inspection and never automatically replayed");
    Probe::StopFlag(s.ops, false); Probe::Notify(s.ops, {});
    Probe::Recover(s.ops, {});
    const auto zero_mutation_count = s.ops.Status().deletes_without_mutation;
    Probe::UncertainResult(s.ops, permanent);
    const auto uncertain_snapshot = s.ops.PendingRecovery();
    const auto outcomes = s.ops.DrainDeleteOutcomes();
    check(uncertain_snapshot.has_uncertain_destructive && uncertain_snapshot.entries.size() == 1 &&
        uncertain_snapshot.entries[0].request.sources == permanent.sources &&
        Probe::Journal(s.ops).find(L"first.txt") != std::wstring::npos &&
        s.ops.Status().deletes_without_mutation == zero_mutation_count &&
        !outcomes.empty() && outcomes.back().uncertain,
        "lost attempted backend result preserves exact recovery journal and is never classified as zero mutation");
    s.ops.UndoFromJson(original_undo);
    Probe::UncertainUndo(s.ops, ordinary);
    const size_t queued_before = Probe::Queued(s.ops);
    s.ops.Undo();
    check(!s.ops.CanUndo() && Probe::Queued(s.ops) == queued_before,
        "uncertain Undo deletion disables the entry and never queues a destructive replay");
    s.quickPreview.Close();
    SetWindowLongPtrW(owner, GWLP_USERDATA, 0); s.hwnd = nullptr;
    DestroyWindow(owner); OleUninitialize();
    std::cout << (failures ? "[FAIL] " : "[PASS] ") << "isolated delete integration; no ShellClient start, scan, filesystem delete or preferences write\n";
    return failures ? 1 : 0;
}
