#pragma once
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; struct DetailsPanelView; }
namespace app { struct FolderSizeValue; }
void FillDetailsFolderSize(ui::DetailsPanelView& details, const app::FolderSizeValue& value);
void FillFolderSizes(AppState& state, ui::WindowViewModel& vm);
}
