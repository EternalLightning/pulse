// app_ops_ui.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
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
        return _wcsicmp(entry.name.c_str(), tab.pending_created_name.c_str()) == 0;
    });
    if (found == tab.snapshot->end()) return;
    const std::wstring name = found->name;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot);
    if (tab.sort_column == ui::SortColumn::Mtime) {
        std::stable_sort(entries->begin(), entries->end(), [&](const auto& left, const auto& right) {
            return app::EntryLess(left, right, tab.sort_column, tab.sort_direction, tab.current_path);
        });
    } else {
        const auto offset = std::distance(tab.snapshot->begin(), found);
        std::rotate(entries->begin() + offset, entries->begin() + offset + 1, entries->end());
    }
    tab.SetSnapshot(std::move(entries));
    tab.order_held = true;
    tab.RemapSelection({name}, name);
    tab.pending_created_name.clear();
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
        RevealPendingCreatedItem(s, *tab);
    });
}

bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    s.ops.Submit(std::move(request));
    return true;
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
    for (const auto& it : b.items) {
        if (it.exists) req.sources.push_back(it.path);
    }
    if (req.sources.empty()) return;
    if (!SubmitWithConflictResolution(s, std::move(req))) return;
    // Cut batches are consumed by release; copy batches too (default per ui.md §7.4).
    s.tray.RemoveBatch(idx);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release newest tray batch, else paste from the system clipboard.
void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    if (!s.tray.batches().empty()) {
        size_t idx = s.tray.batches().size() - 1;
        if (s.tray.batches()[idx].move_intent) s.cutPaths.clear();
        ReleaseTrayBatch(s, idx);
        return;
    }
    ops::ClipboardData cb;
    if (ops::ReadClipboard(cb)) {
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
    }
}

void DeleteSelected(AppState& s, bool permanent) {
    if(DeferContentSelection(s,[=](AppState& v){DeleteSelected(v,permanent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->net_readonly) return;
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
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
void RestoreSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
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
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
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
void PinAndShowOperationWindow(AppState& s) {
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
    if (paths.size() < 2) return;
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
    if (allow_conflict_dialog) PresentDeleteConfirmation(s);
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

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
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, *conflict, s.darkMode, s.accentColor);
            s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
        }
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId &&
        !s.operationPinnedByUser) {
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
        if (status.last_error == L"已取消") {
            s.operationWindow->Hide();
            return;
        }
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
            const bool no_access = err.find(L"没有权限") != std::wstring::npos
                || err.find(L"拒绝访问") != std::wstring::npos
                || err.find(L"Access is denied") != std::wstring::npos
                || err == L"create failed";
            if (no_access) {
                message = l10n::Get(folder || file
                    ? l10n::StringId::FolderNoWritePermission
                    : l10n::StringId::RenameNoPermission);
            } else if (err == L"目标名称已存在") {
                message = l10n::Get(l10n::StringId::RenameTargetExists);
            } else if (err == L"名称无效") {
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
