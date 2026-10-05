#include "folder_sizes_ui.h"
#include "app_state.h"
#include "session.h"
#include "../common/text_format.h"
#include "../common/localization.h"

namespace pulse {
void FillFolderSizes(AppState& s, ui::WindowViewModel& vm) {
    struct Row { ui::PaneViewModel* pane; int index; std::wstring path; };
    std::vector<Row> rows;
    std::vector<app::FolderSizeRequest> requests;
    std::vector<std::wstring> roots;
    for (auto& slot : vm.pane_slots) {
        auto& pane = slot.pane;
        pane.folder_size_labels.clear();
        pane.folder_size_actions.clear();
        if (!ui::ShowsFolderSize(pane.view_mode) || pane.loading || !pane.is_file_system ||
            pane.is_search || pane.is_recycle) continue;
        const auto list = s.renderer.PaneListRect(pane, slot.rect);
        ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x, pane.scroll_y, s.scale);
        const auto [first, last] = layout.VisibleRange();
        for (int i = std::max(0, first); i <= last; ++i) {
            const int source = pane.SourceIndex(i);
            if (source < 0) continue;
            std::wstring path;
            if (pane.snapshot) {
                const auto& entry = (*pane.snapshot)[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.full_path.empty() ? pane.path + L"\\" + entry.name : entry.full_path;
            } else {
                const auto& entry = pane.entries[static_cast<size_t>(source)];
                if (!entry.is_dir) continue;
                path = entry.path;
            }
            if (path.empty() || fs::IsVirtualPath(path)) continue;
            rows.push_back({&pane, source, path});
            requests.push_back({path, !fs::IsUncPath(path)});
        }
        if (!pane.path.empty() && !fs::IsVirtualPath(pane.path)) roots.push_back(pane.path);
    }
    if (!s.isolatedTest && !s.shot.active)
        s.folderSizes.SetCachePath([] {
            const auto dir = app::GetPulseDataDir();
            return dir.empty() ? std::wstring() : dir + L"\\folder_sizes.json";
        });
    s.folderSizes.SetIndexEnabled(!s.isolatedTest && !s.shot.active);
    s.folderSizes.Sync(std::move(requests), std::move(roots));
    using S = app::FolderSizeState;
    using I = l10n::StringId;
    for (const auto& row : rows) {
        const auto value = s.folderSizes.Get(row.path);
        std::wstring text = value.has_value ? format::ByteSize(value.bytes) : L"";
        auto suffix = [&](I id) {
            if (!text.empty()) text += L" · ";
            text += l10n::Get(id);
        };
        switch (value.state) {
        case S::Indexed: suffix(I::FolderSizeIndexed); break;
        case S::Manual: suffix(I::FolderSizeCalculate); break;
        case S::Calculating: suffix(I::Calculating); break;
        case S::Updating: suffix(I::FolderSizeUpdating); break;
        case S::Partial: suffix(I::FolderSizePartial); break;
        case S::Unavailable: suffix(I::FolderSizeUnavailable); break;
        case S::Cached: suffix(I::FolderSizeCached); break;
        default: break;
        }
        row.pane->folder_size_labels.emplace(row.index, ui::FolderSizeLabel{std::move(text), value.state == S::Manual});
        if (value.state == S::Manual || value.state == S::Unavailable ||
            value.state == S::Cached || value.state == S::Partial || value.state == S::Indexed)
            row.pane->folder_size_actions.insert(row.index);
    }
    for (const auto& slot : vm.pane_slots) if (slot.focused) {
        vm.pane.folder_size_labels = slot.pane.folder_size_labels;
        vm.pane.folder_size_actions = slot.pane.folder_size_actions;
    }
}
}
