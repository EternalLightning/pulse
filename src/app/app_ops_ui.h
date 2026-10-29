// app_ops_ui.h — Delete/tray/paste/conflict/recycle operations UI.
#pragma once
#include "app_runtime.h"

namespace pulse {
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request);
void ReleaseTrayBatch(AppState& s, size_t idx);
void PasteIntoCurrent(AppState& s);
void DeleteSelected(AppState& s, bool permanent);
void RestoreSelected(AppState& s);
void EmptyRecycleBin(AppState& s);
void CollectToTray(AppState& s, bool move_intent);
void ShowBatchRename(AppState& s);
void PinAndShowOperationWindow(AppState& s);
void EnsureOperationWindow(AppState& s);
void InitializeOperationCenter(AppState& s);
void ToggleOperationCenter(AppState& s);
void UpdateOperationCenter(AppState& s);
std::vector<ops::OpStatus> CollectOperationCenterTasks(AppState& s);
AppState* ResolveOperationCenterTask(AppState& s, uint64_t& task);
void SyncOperationCenters(AppState& s);
void UpdateOperationCenterTheme(AppState& s);
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog);
void QueueCreatedItemReveal(AppState& s, const std::wstring& path);
void QueueTransferredItemsReveal(AppState& s, const std::vector<std::wstring>& paths);
void RevealPendingCreatedItem(AppState& s, app::Tab& tab);
void PresentDeleteConfirmation(AppState& s);
ui::ConfirmDialogSpec BuildDeleteConfirmationSpec(const ops::DeleteConfirmation& confirmation);
} // namespace pulse
