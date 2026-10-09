#include "drive_label_rename.h"
#include "app_internal.h"

namespace pulse {

void ApplyDriveLabelRename(AppState& s, const std::wstring& root, const std::wstring& label) {
    const auto normalized = fs::NormalizePath(root);
    const auto name = app::DriveDisplayName(root, label);
    for (auto& drive : s.sidebar.drives) {
        if (path::EqualInsensitive(fs::NormalizePath(drive.path), normalized)) {
            drive.label = name;
            break;
        }
    }
    s.home_catalog.UpdateDriveLabel(root, label);
    s.store.MarkDirty(L"");
    ForEachPane(s, [&](app::Pane& pane) {
        auto* tab = pane.ActiveTab();
        if (!tab || !tab->current_path.empty() || !tab->snapshot) return;
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        for (auto& entry : *entries) {
            if (path::EqualInsensitive(fs::NormalizePath(entry.full_path), normalized)) {
                entry.name = name;
                tab->SetSnapshot(entries);
                break;
            }
        }
    });
    RefreshPath(s, L"", RefreshReason::OperationCompleted);
}

} // namespace pulse
