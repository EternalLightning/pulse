// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// app_ops_ui.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_conflict_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "entry_sort.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "clipboard_paste_choice.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
void RevealPendingCreatedItem(AppState& s, app::Tab& tab) {
    if (tab.pending_created_name.empty() || tab.pending_generation != 0 || !tab.snapshot ||
        tab.snapshot_path != tab.current_path) return;
    const auto found = std::find_if(tab.snapshot->begin(), tab.snapshot->end(), [&](const auto& entry) {
        return entry.name == tab.pending_created_name;
    });
    if (found == tab.snapshot->end()) return;
    auto names = tab.pending_created_names;
    if (names.empty()) names.push_back(found->name);
    if (names.size() > 1) {
        std::unordered_set<std::wstring> remaining(names.begin(), names.end());
        std::vector<std::wstring> actual_names;
        for (const auto& entry : *tab.snapshot) {
            if (remaining.erase(entry.name)) actual_names.push_back(entry.name);
        }
        if (!remaining.empty()) return;
        names = std::move(actual_names);
    } else names.front() = found->name;
    const std::wstring name = found->name;
    tab.RemapSelection(names, name);
    tab.pending_created_name.clear();
    tab.pending_created_names.clear();
    if (ActiveTab(s) == &tab) {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        if (pane.ViewIndex(tab.selected_index) < 0) ClearPaneFilter(s);
        EnsureRowVisible(s, tab, tab.selected_index);
        s.scrollTargetY = tab.scroll_y;
        s.scrollAnimating = false;
    }
}

void QueueCreatedItemReveal(AppState& s, const std::wstring& path) {
    const std::wstring parent = fs::ParentPath(path);
    const std::wstring name = PathFindFileNameW(path.c_str());
    if (parent.empty() || name.empty()) return;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || _wcsicmp(tab->current_path.c_str(), parent.c_str()) != 0) return;
        tab->pending_created_name = name;
        tab->pending_created_names = {name};
        RevealPendingCreatedItem(s, *tab);
    });
}

void QueueTransferredItemsReveal(AppState& s, const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> folders;
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab) return;
        std::vector<std::wstring> names;
        for (const auto& path : paths) {
            const auto parent = fs::ParentPath(path);
            const std::wstring name = PathFindFileNameW(path.c_str());
            if (!parent.empty() && !name.empty() &&
                _wcsicmp(tab->current_path.c_str(), parent.c_str()) == 0 &&
                std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        if (names.empty()) return;
        tab->pending_created_name = names.front();
        tab->pending_created_names = std::move(names);
        // Wait for a snapshot taken after the successful transfer, including replacements.
        if (std::find(folders.begin(), folders.end(), tab->current_path) == folders.end())
            folders.push_back(tab->current_path);
    });
    for (const auto& folder : folders) RefreshPath(s, folder, RefreshReason::OperationCompleted);
}

bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    return s.ops.Submit(std::move(request)) != 0;
}

// Release one tray batch into the current folder through the ops layer.
void ReleaseTrayBatch(AppState& s, size_t idx) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || idx >= s.tray.batches().size()) return;
    if (fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    const app::TrayBatch& b = s.tray.batches()[idx];
    ops::OpRequest req;
    req.type = b.move_intent ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = tab->current_path;
    std::vector<size_t> submitted;
    for (size_t i = 0; i < b.items.size(); ++i) {
        if (b.items[i].exists) {
            req.sources.push_back(b.items[i].path);
            submitted.push_back(i);
        }
    }
    if (req.sources.empty()) return;
    const bool move_intent = b.move_intent;
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    s.trayTransfers[task].move_intent = move_intent;
    for (auto it = submitted.rbegin(); it != submitted.rend(); ++it) s.tray.RemoveItem(idx, *it);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release newest tray batch, else paste from the system clipboard.
void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || tab->net_readonly) return;
    if (app::IsDeviceTab(tab)) {
        if (CanPasteIntoCurrent(s)) app::QueueDeviceOperation(s, L"paste",
            {app::DeviceRegistry::Instance().FolderShellId(tab->current_path)});
        return;
    }
    if (fs::IsVirtualPath(tab->current_path)) return;
    if (!CanPasteIntoCurrent(s)) return;
    const uint32_t observed_sequence = GetClipboardSequenceNumber();
    ops::ClipboardData cb;
    const bool clipboard_paths = ops::ReadClipboard(cb);
    const bool shell_files = ops::ClipboardHasShellFiles();
    const auto source = app::ChooseClipboardPasteSource(clipboard_paths, cb, shell_files,
        !s.tray.batches().empty(), observed_sequence, GetClipboardSequenceNumber());
    if (source == app::ClipboardPasteSource::None) return;
    if (source == app::ClipboardPasteSource::Tray) {
        size_t idx = s.tray.batches().size() - 1;
        if (s.tray.batches()[idx].move_intent) s.cutPaths.clear();
        ReleaseTrayBatch(s, idx);
        return;
    }
    if (source == app::ClipboardPasteSource::Paths) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        if (SubmitWithConflictResolution(s, std::move(req)) && cb.cut) {
            s.cutPaths.clear();
            s.pendingCutClipboardSequence = cb.sequence;
            s.pendingCutClipboardPaths = cb.paths;
            s.completedCutClipboardPaths.clear();
        }
    } else if (source == app::ClipboardPasteSource::Shell) {
        // Shell providers can block while materializing virtual files. The I/O
        // worker is an STA, so OleGetClipboard and the folder drop target stay
        // off the UI thread and in one apartment.
        const std::wstring destination = tab->current_path;
        const uint32_t sequence = observed_sequence;
        const auto mailbox = s.clipboard_paste_mailbox;
        const bool queued = s.worker.EnqueueIo([destination, sequence, mailbox] {
            const bool pasted = ops::PasteShellClipboard(destination, sequence);
            // Clipboard replacement is a stale request, not a user-visible failure.
            if (pasted || GetClipboardSequenceNumber() == sequence)
                if (mailbox) mailbox->Publish(pasted);
        });
        if (!queued && mailbox) mailbox->Publish(false);
    }
}

void DeleteSelected(AppState& s, bool permanent) {
    if(DeferContentSelection(s,[=](AppState& v){DeleteSelected(v,permanent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->net_readonly || app::HasDeviceSelection(*tab)) return;
    if (IsRecycleTab(tab)) {
        ops::OpRequest req;
        req.type = ops::OpType::RealDelete;
        if (tab->snapshot) {
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
                const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(index));
                if (entry.recycle_path.empty()) continue;
                req.sources.push_back(entry.recycle_path);
                req.delete_targets.push_back({entry.full_path.empty() ? entry.recycle_path : entry.full_path,
                    {entry.recycle_path}, true});
            }
        }
        if (req.sources.empty()) return;
        s.ops.Submit(std::move(req));
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty() || std::any_of(paths.begin(), paths.end(), path::IsDriveRootPath)) return;
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
void RestoreSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !IsRecycleTab(tab) || !tab->snapshot) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.collision_policy = ops::CollisionPolicy::KeepBoth;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto& entry = tab->EntryAt(static_cast<size_t>(index));
        if (entry.full_path.empty() || entry.recycle_path.empty()) continue;
        req.sources.push_back(entry.full_path);
        req.recycle_paths.push_back(entry.recycle_path);
    }
    if (!req.sources.empty()) s.ops.Submit(std::move(req));
}

void EmptyRecycleBin(AppState& s) {
    ops::OpRequest req;
    req.type = ops::OpType::EmptyRecycle;
    req.delete_origin = ops::DeleteOrigin::EmptyRecycle;
    s.ops.Submit(std::move(req));
}
void CollectToTray(AppState& s, bool move_intent) {
    if(DeferContentSelection(s,[=](AppState& v){CollectToTray(v,move_intent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    if (app::HasDeviceSelection(*tab)) {
        if (!move_intent) app::QueueDeviceOperation(s, L"copy", app::DeviceSelectedIds(*tab));
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (std::any_of(paths.begin(), paths.end(), path::IsDriveRootPath)) return;
    if (!paths.empty()) {
        s.tray.Collect(paths, move_intent);
        // Mirror the cut state onto the list rows (ui.md §5.2 rule 6).
        s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
        // Interop with Explorer: mirror the collection onto the system clipboard.
        std::vector<std::wstring> cbPaths;
        cbPaths.reserve(paths.size());
        for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
        ops::WriteClipboard(cbPaths, move_intent);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}
void EnsureOperationWindow(AppState& s) {
    if (s.operationWindow || s.exit_requested || s.ui_schedule.stopped || !s.hwnd) return;
    const HWND hwnd = s.hwnd;
    s.operationWindow = std::make_unique<ui::FileOperationWindow>();
    ui::FileOperationCallbacks operation_callbacks;
    operation_callbacks.cancel = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.CancelCurrent();
    };
    operation_callbacks.pause = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.PauseCurrent();
    };
    operation_callbacks.resume = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) state->ops.ResumeCurrent();
    };
    operation_callbacks.dismiss = [hwnd] {
        if (AppState* state = GetAppState(hwnd)) {
            state->operationDismissedTaskId = state->ops.Status().task_id;
            state->operationPinnedByUser = false;
        }
    };
    if (!s.operationWindow->Create(hwnd, std::move(operation_callbacks))) {
        s.operationWindow.reset();
        return;
    }
    s.operationWindow->SetTheme(s.darkMode, s.accentColor);
}
void PinAndShowOperationWindow(AppState& s) {
    EnsureOperationWindow(s);
    if (!s.operationWindow) return;
    const ops::OpStatus status = s.ops.Status();
    if (!status.active && status.summary.empty() && status.last_error.empty()) return;
    s.operationDismissedTaskId = 0;
    s.operationPinnedByUser = true;
    s.operationAutoShown = true;
    s.operationWindow->Update(status);
    s.operationWindow->Show(true);
}

void ShowBatchRename(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){ShowBatchRename(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.size() < 2 || std::any_of(paths.begin(), paths.end(), path::IsDriveRootPath)) return;
    const auto result = ui::ShowBatchRenameDialog(s.hwnd, paths, s.darkMode, s.accentColor);
    if (!result.accepted) return;
    ops::OpRequest req;
    req.type = ops::OpType::BatchRename;
    for (const auto& item : result.items) {
        if (item.status != app::BatchRenameStatus::Ok) continue;
        req.sources.push_back(item.source_path);
        req.new_names.push_back(item.new_name);
    }
    if (req.sources.empty()) return;
    tab->pending_selected_names = req.new_names;
    if (!req.new_names.empty()) tab->pending_selected_name = req.new_names.front();
    s.ops.Submit(std::move(req));
}
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    UpdateOperationCenter(s);
    if (allow_conflict_dialog) PresentDeleteConfirmation(s);
    const auto pending_status = s.ops.Status();
    if (pending_status.active || !pending_status.summary.empty() || !pending_status.last_error.empty())
        EnsureOperationWindow(s);
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

    if (s.permissionOperationPromptActive || (!status.active &&
        status.phase == ops::OpPhase::Failed && status.elevation_retry_available &&
        s.ops.IsElevationFailureCurrent(status.task_id))) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
        s.operationPinnedByUser = false;
        s.operationStartedAt = now;
        s.operationFinishedAt = {};
    }

    if (allow_conflict_dialog) {
        if (const auto conflict = s.ops.PendingConflict();
            conflict && conflict->token != s.conflictUiToken) {
            s.conflictUiToken = conflict->token;
            if (conflict->rows.size() > 1 && !conflict->link_confirmation) {
                using I = l10n::StringId;
                ui::BatchConflictLabels labels;
                labels.title = l10n::Get(I::OpConflictTitle);
                labels.instruction = l10n::Get(I::BatchConflictInstruction);
                labels.source = l10n::Get(I::OpSource);
                labels.destination = l10n::Get(I::OpExisting);
                labels.replace = l10n::Get(I::OpReplaceFile);
                labels.skip = l10n::Get(I::OpSkipFile);
                labels.keep_both = l10n::Get(I::OpKeepBoth);
                labels.apply_all = l10n::Get(I::BatchConflictApplyAll);
                labels.confirm = l10n::Get(I::BatchConflictContinue);
                labels.cancel = l10n::Get(I::Cancel);
                labels.folder = l10n::Get(I::TypeFolder);
                labels.reparse = l10n::Get(I::BatchConflictLink);
                labels.unknown = l10n::Get(I::BatchConflictUnknown);
                const auto result = ui::ShowBatchFileConflictDialog(s.hwnd, *conflict, labels, s.darkMode, s.accentColor);
                if (result) s.ops.ResolveConflicts(conflict->token, *result);
                else s.ops.ResolveConflict(conflict->token, ops::ConflictChoice::Cancel, false);
            } else {
                const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                    s.hwnd, *conflict, s.darkMode, s.accentColor);
                s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
            }
        }
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId &&
        !s.operationPinnedByUser) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (!IsWindowVisible(s.hwnd)) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active) {
        if (status.phase == ops::OpPhase::WaitingForConflict ||
            status.phase == ops::OpPhase::WaitingForDeleteConfirmation) return;
        const bool show_now = status.type == ops::OpType::EmptyRecycle;
        if (!s.operationAutoShown &&
            (show_now || now - s.operationStartedAt >= std::chrono::milliseconds(2000))) {
            s.operationWindow->Show(false);
            s.operationAutoShown = true;
        }
        return;
    }

    if (status.phase == ops::OpPhase::Cancelled) {
        s.operationWindow->Hide();
        return;
    }
    if (s.operationPinnedByUser) return;

    if (status.phase == ops::OpPhase::Completed) {
        if (s.operationWindow->IsVisible()) {
            if (s.operationFinishedAt.time_since_epoch().count() == 0)
                s.operationFinishedAt = now;
            if (now - s.operationFinishedAt >= std::chrono::milliseconds(600))
                s.operationWindow->Hide();
        }
    } else if (status.phase == ops::OpPhase::Failed) {
        const bool simple = status.type == ops::OpType::CreateFolder
                         || status.type == ops::OpType::CreateTextFile
                         || status.type == ops::OpType::Rename
                         || status.type == ops::OpType::BatchRename;
        if (simple) {
            if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
            if (status.task_id == 0 || status.task_id == s.operationDismissedTaskId)
                return;
            s.operationDismissedTaskId = status.task_id;
            if (status.type == ops::OpType::CreateFolder ||
                status.type == ops::OpType::CreateTextFile)
                s.pendingRenameName.clear();
            const bool folder = status.type == ops::OpType::CreateFolder;
            const bool file = status.type == ops::OpType::CreateTextFile;
            const std::wstring title = l10n::Get(folder ? l10n::StringId::CannotCreateFolder
                : file ? l10n::StringId::CannotCreateTextFile : l10n::StringId::CannotRename);
            std::wstring message;
            const std::wstring& err = status.last_error;
            const HRESULT failure = status.failure_hr;
            const bool no_access = failure == E_ACCESSDENIED ||
                failure == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD);
            if (no_access) {
                message = l10n::Get(folder || file
                    ? l10n::StringId::FolderNoWritePermission
                    : l10n::StringId::RenameNoPermission);
            } else if (failure == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
                       failure == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)) {
                message = l10n::Get(l10n::StringId::RenameTargetExists);
            } else if (failure == HRESULT_FROM_WIN32(ERROR_INVALID_NAME)) {
                message = l10n::Get(l10n::StringId::InvalidName);
            } else if (!err.empty()) {
                message = err;
            } else {
                message = l10n::Get(l10n::StringId::OperationFailedMessage);
            }
            s.notification_toast.ShowError(s.hwnd, title, std::move(message));
            return;
        }
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(true);
    }
}

} // namespace pulse
