#pragma once
namespace pulse {
struct AppState;
struct FolderSizesUiCache;
namespace ui { struct WindowViewModel; struct DetailsPanelView; }
namespace app { struct FolderSizeValue; }
void FillDetailsFolderSize(ui::DetailsPanelView& details, const app::FolderSizeValue& value);
// Synchronize visible interests before drawing, then fill their current values.
void PrepareFolderSizes(AppState& state, ui::WindowViewModel& vm);
// Read cached values without starting work or changing visible interests.
void FillFolderSizes(AppState& state, ui::WindowViewModel& vm);
void QueueFolderSizeSort(AppState& state);
// Applies cached totals only; defers during pointer gestures and inline rename.
bool ApplyFolderSizeSort(AppState& state);
#ifdef PULSE_FOLDER_SIZES_TESTING
struct FolderSizesUiStats {
    unsigned request_rebuilds = 0;
    unsigned syncs = 0;
    unsigned sort_passes = 0;
};
FolderSizesUiStats FolderSizesUiStatsForTest(const AppState& state);
#endif
}
