#pragma once
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; struct DetailsPanelView; }
namespace app { struct FolderSizeValue; }
void FillDetailsFolderSize(ui::DetailsPanelView& details, const app::FolderSizeValue& value);
// Synchronize visible interests before drawing, then fill their current values.
void PrepareFolderSizes(AppState& state, ui::WindowViewModel& vm);
// Read cached values without starting work or changing visible interests.
void FillFolderSizes(AppState& state, ui::WindowViewModel& vm);
}
