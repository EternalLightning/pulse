// column_view_model.h — Per-tab listing state of the column (Miller) view.
#pragma once
#include "../fs/fs_snapshot.h"
#include "entry_sort.h"
#include "../ui/smooth_scroll.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui { struct ColumnStripView; }

namespace pulse::app {

struct Tab;

struct ColumnStripColumn {
    std::wstring path;                 // normalized folder path
    std::wstring title;                // caption override (drive label), may be empty
    fs::SnapshotPtr snapshot;          // last listing (may be stale while refreshing)
    std::shared_ptr<const std::vector<int>> rows; // visible source indices
    fs::SnapshotPtr rows_snapshot;     // snapshot `rows` was built from
    bool rows_show_hidden = false;
    bool rows_show_protected = false;
    ui::SortColumn rows_sort_column = ui::SortColumn::Name;
    ui::SortDirection rows_sort_direction = ui::SortDirection::Asc;
    FolderSortMode rows_folder_sort = FolderSortMode::FoldersFirst;
    std::wstring highlight_name;       // entry on the navigation path, if any
    int highlight_row = -1;
    uint64_t generation = 0;           // worker generation still awaited (0 = none)
    uint64_t applied_generation = 0;
    uint64_t notify_watermark = 0; // Results issued before a newer notification patch.
    bool requested = false;
    bool error = false;
    float scroll_dip = 0.0f;
    ui::SmoothScroll scroll_motion;
    bool auto_scroll = true;
};

struct ColumnStripState {
    std::wstring for_path;                     // current_path the ancestors belong to
    std::vector<ColumnStripColumn> ancestors;  // farthest -> parent
    ColumnStripColumn child;                   // selected folder (path empty = none)
    float scroll_from_right_dip = 0.0f;        // ancestor strip scroll; 0 = parent visible
    ui::SmoothScroll horizontal_motion;

    bool Empty() const { return for_path.empty() && ancestors.empty() && child.path.empty(); }
};

// Column view data for the renderer (cheap: shares snapshots and row lists).
void FillColumnStripView(ui::ColumnStripView& out, const Tab& tab);

} // namespace pulse::app
