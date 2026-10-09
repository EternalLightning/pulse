// app_column_view.h — Column (Miller) view: listing sync and input handling.
#pragma once
#include "app_runtime.h"

namespace pulse {

// Keeps every visible pane's column listings in step with its folder and
// selection. Cheap when nothing changed; starts async loads otherwise.
void SyncColumnStrips(AppState& s);
// A directory mutation updates or invalidates every visible column projection.
void InvalidateColumnStripPath(AppState& s, const std::wstring& path,
                               fs::SnapshotPtr fresh = {});
// Called for every worker result before pane handling.
void NoteColumnStripResult(AppState& s, const app::WorkResult& res);

// Mouse-down on column-view regions (toggle button, rows, dividers).
// Returns true when consumed.
bool HandleColumnStripMouseDown(AppState& s, const ui::WindowViewModel& vm,
                                const ui::HitTestResult& hit, int x);
bool UpdateColumnStripResize(AppState& s, int x);
// Double-click on a divider restores that column's default width.
void ResetColumnStripWidth(AppState& s, const ui::HitTestResult& hit);
void EndColumnStripResize(AppState& s);
// Double-click right after a column click changed the folder: swallow it so
// the second click does not open whatever moved under the cursor.
bool ColumnStripSwallowDoubleClick(const AppState& s);
bool HandleColumnStripWheel(AppState& s, const ui::WindowViewModel& vm,
                            const ui::HitTestResult& hit, int wheel_delta);
// Left/Right in the column view: go to the parent / enter the selected folder.
bool HandleColumnArrowKey(AppState& s, bool right);
void ToggleColumnLayout(AppState& s, int pane_index);

} // namespace pulse
