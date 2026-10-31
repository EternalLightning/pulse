// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "folder_sizes_ui.h"
#include "app_state.h"
#include "session.h"
#include "../common/text_format.h"
#include "../common/localization.h"
#include "entry_sort.h"
#include <numeric>

namespace pulse {
struct FolderSizesUiCache {
    struct Interest {
        fs::SnapshotPtr snapshot;
        std::wstring path;
        bool enabled = false, all = false;
        std::vector<int> sources;
        std::vector<app::FolderSizeRequest> requests;
    };
    struct SortStamp {
        std::weak_ptr<void> owner;
        fs::SnapshotPtr snapshot;
        std::wstring path;
        ui::SortDirection direction = ui::SortDirection::Asc;
        int folders = 0;
        uint64_t revision = 0;
    };
    std::vector<Interest> interests;
    std::vector<app::FolderSizeRequest> requests;
    std::vector<std::wstring> roots;
    std::wstring details_path;
    std::vector<SortStamp> sorts;
    uint64_t revision = 0;
#ifdef PULSE_FOLDER_SIZES_TESTING
    FolderSizesUiStats stats;
#endif
};
void FillDetailsFolderSize(ui::DetailsPanelView& details, const app::FolderSizeValue& value) {
    using S = app::FolderSizeState;
    using I = l10n::StringId;
    details.size_pending = value.state == S::Calculating || value.state == S::Updating;
    details.size_text = value.has_value ? format::ByteSize(value.bytes, true) : L"";
    if (value.state == S::Partial || value.state == S::Cached || value.state == S::Unavailable) {
        if (!details.size_text.empty()) details.size_text += L" · ";
        details.size_text += l10n::Get(value.state == S::Partial ? I::FolderSizePartial :
            value.state == S::Cached ? I::FolderSizeCached : I::FolderSizeUnavailable);
    }
    details.contains_text.clear();
    if (value.counts_valid) {
        wchar_t contains[96];
        swprintf_s(contains, l10n::Get(I::ContainsFormat).c_str(), value.files, value.dirs);
        details.contains_text = contains;
        if (value.incomplete || value.state == S::Partial || value.state == S::Cached || value.state == S::Updating) {
            details.contains_text += L" · ";
            details.contains_text += l10n::Get(value.incomplete || value.state == S::Partial ?
                I::FolderSizePartial : value.state == S::Updating ? I::FolderSizeUpdating : I::FolderSizeCached);
        }
    }
}

namespace {
FolderSizesUiCache& Cache(AppState& s) {
    if (!s.folder_sizes_ui_cache) s.folder_sizes_ui_cache = std::make_shared<FolderSizesUiCache>();
    return *s.folder_sizes_ui_cache;
}
bool Sortable(const app::Tab& tab) {
    return tab.sort_column == ui::SortColumn::Size && !tab.loading && tab.snapshot &&
        tab.snapshot_path == tab.current_path && !tab.content_results &&
        tab.archive_file.empty() && !tab.current_path.empty() && !fs::IsVirtualPath(tab.current_path);
}
bool SortCurrent(const FolderSizesUiCache& cache, const app::Pane& pane, int folders) {
    const auto& tab = pane.view;
    return std::any_of(cache.sorts.begin(), cache.sorts.end(), [&](const auto& stamp) {
        return stamp.owner.lock() == pane.lifetime_token && stamp.snapshot == tab.snapshot &&
            stamp.path == tab.current_path && stamp.direction == tab.sort_direction &&
            stamp.folders == folders && stamp.revision == cache.revision;
    });
}
void ScheduleChangedSort(AppState& s) {
    auto* layout = s.window_tabs.Active();
    if (!layout) return;
    auto& cache = Cache(s);
    std::erase_if(cache.sorts, [&](const auto& stamp) {
        const auto owner = stamp.owner.lock();
        return !owner || std::none_of(layout->panes.begin(), layout->panes.end(),
            [&](const auto& pane) { return pane && pane->lifetime_token == owner; });
    });
    const int folders = s.appPrefs.folder_sort_mode;
    for (const auto& pane : layout->panes) if (pane && Sortable(pane->view) && !SortCurrent(cache, *pane, folders)) {
        s.folder_size_sort_pending = true;
        return;
    }
}
void RememberSort(FolderSizesUiCache& cache, const app::Pane& pane, int folders) {
    auto stamp = std::find_if(cache.sorts.begin(), cache.sorts.end(), [&](const auto& previous) {
        return previous.owner.lock() == pane.lifetime_token;
    });
    if (stamp == cache.sorts.end()) {
        cache.sorts.emplace_back();
        stamp = std::prev(cache.sorts.end());
    }
    *stamp = {pane.lifetime_token, pane.view.snapshot, pane.view.current_path,
        pane.view.sort_direction, folders, cache.revision};
}
std::wstring FolderPath(const ui::PaneViewModel& pane, int source) {
    if (source < 0 || static_cast<size_t>(source) >= (pane.snapshot ? pane.snapshot->size() : pane.entries.size())) return {};
    if (pane.snapshot) {
        const auto& entry = (*pane.snapshot)[static_cast<size_t>(source)];
        if (!entry.is_dir) return {};
        return entry.full_path.empty() ? pane.path + L"\\" + entry.name : entry.full_path;
    }
    const auto& entry = pane.entries[static_cast<size_t>(source)];
    return entry.is_dir ? entry.path : std::wstring();
}
void FillFolderSizesImpl(AppState& s, ui::WindowViewModel& vm, bool prepare_requests) {
    struct Row { ui::PaneViewModel* pane; int index; std::wstring path; };
    std::vector<Row> rows;
    auto& cache = Cache(s);
    bool interests_changed = false;
    if (prepare_requests && cache.interests.size() != vm.pane_slots.size()) {
        cache.interests.resize(vm.pane_slots.size());
        interests_changed = true;
    }
    for (size_t slot_index = 0; slot_index < vm.pane_slots.size(); ++slot_index) {
        auto& slot = vm.pane_slots[slot_index];
        auto& pane = slot.pane;
        pane.folder_size_labels.clear();
        pane.folder_size_actions.clear();
        const bool sort_sizes = pane.sort_column == ui::SortColumn::Size;
        const bool enabled = (ui::ShowsFolderSize(pane.view_mode) || sort_sizes) && !pane.loading &&
            pane.is_file_system && !pane.is_search && !pane.is_recycle;
        std::vector<int> visible_sources;
        if (enabled) {
            const auto list = s.renderer.PaneListRect(pane, slot.rect);
            ui::ViewLayout layout(pane.view_mode, list, pane.EntryCount(), pane.scroll_x, pane.scroll_y, s.scale);
            const auto [first, last] = layout.VisibleRange();
            for (int i = std::max(0, first); i <= last; ++i) {
                const int source = pane.SourceIndex(i);
                if (!sort_sizes) visible_sources.push_back(source);
                if (!ui::ShowsFolderSize(pane.view_mode)) continue;
                auto path = FolderPath(pane, source);
                if (!path.empty() && !fs::IsVirtualPath(path)) rows.push_back({&pane, source, std::move(path)});
            }
        }
        if (!prepare_requests) continue;
        auto& interest = cache.interests[slot_index];
        // Immutable snapshots own the directory generation. Viewport/filter
        // changes only affect visible interests; size sorting owns all folders.
        if (interest.enabled == enabled && (!enabled || (pane.snapshot && interest.snapshot == pane.snapshot &&
            interest.path == pane.path && interest.all == sort_sizes &&
            (sort_sizes || interest.sources == visible_sources)))) continue;
        interests_changed = true;
        interest = {};
        interest.enabled = enabled;
        if (!enabled) continue;
        interest.snapshot = pane.snapshot;
        interest.path = pane.path;
        interest.all = sort_sizes;
        interest.sources = std::move(visible_sources);
        const auto add = [&](int source) {
            auto path = FolderPath(pane, source);
            if (!path.empty() && !fs::IsVirtualPath(path))
                interest.requests.push_back({path, !fs::IsUncPath(path)});
        };
        if (sort_sizes) {
            const size_t count = pane.snapshot ? pane.snapshot->size() : pane.entries.size();
            for (size_t i = 0; i < count; ++i) add(static_cast<int>(i));
        } else for (int source : interest.sources) add(source);
#ifdef PULSE_FOLDER_SIZES_TESTING
        ++cache.stats.request_rebuilds;
#endif
    }
    const auto details_path = vm.details.is_dir && !fs::IsVirtualPath(vm.details.path)
        ? vm.details.path : std::wstring();
    if (prepare_requests) {
        interests_changed |= cache.details_path != details_path;
        if (interests_changed) {
            cache.requests.clear();
            cache.roots.clear();
            cache.details_path = details_path;
            for (const auto& interest : cache.interests) if (interest.enabled) {
                cache.requests.insert(cache.requests.end(), interest.requests.begin(), interest.requests.end());
                if (!interest.path.empty() && !fs::IsVirtualPath(interest.path)) cache.roots.push_back(interest.path);
            }
            if (!details_path.empty()) {
                cache.requests.push_back({details_path, true, true});
                cache.roots.push_back(details_path);
            }
        }
        if (interests_changed || s.folderSizes.NeedsWorker()) {
            if (!s.isolatedTest && !s.shot.active)
                s.folderSizes.SetCachePath([] {
                    const auto dir = app::GetPulseDataDir();
                    return dir.empty() ? std::wstring() : dir + L"\\folder_sizes.json";
                });
            s.folderSizes.SetIndexEnabled(!s.isolatedTest && !s.shot.active);
            s.folderSizes.Sync(cache.requests, cache.roots);
#ifdef PULSE_FOLDER_SIZES_TESTING
            ++cache.stats.syncs;
#endif
        }
    }
    using S = app::FolderSizeState;
    using I = l10n::StringId;
    if (!details_path.empty()) {
        FillDetailsFolderSize(vm.details, s.folderSizes.Get(details_path));
    }
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
} // namespace

void PrepareFolderSizes(AppState& s, ui::WindowViewModel& vm) {
    FillFolderSizesImpl(s, vm, true);
    ScheduleChangedSort(s);
}

void FillFolderSizes(AppState& s, ui::WindowViewModel& vm) {
    FillFolderSizesImpl(s, vm, false);
}

void QueueFolderSizeSort(AppState& s) {
    ++Cache(s).revision;
    ScheduleChangedSort(s);
}

bool ApplyFolderSizeSort(AppState& s) {
    if (!s.folder_size_sort_pending || GetTickCount64() < s.folder_size_sort_due) return false;
    if (s.renameIndex >= 0 || s.dragPending || s.scrollbarDragging || s.splitterDragging ||
        s.tabDragging || s.paneDragging || s.dropRow >= 0 || !s.dropDestDir.empty() ||
        (s.menu && s.menu->IsOpen()) ||
        (GetKeyState(VK_LBUTTON) & 0x8000) || (GetKeyState(VK_RBUTTON) & 0x8000)) return false;
    s.folder_size_sort_pending = false;
    s.folder_size_sort_due = GetTickCount64() + 750;
    auto* layout = s.window_tabs.Active();
    if (!layout) return false;
    const auto folders = app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode);
    auto& cache = Cache(s);
    bool changed = false;
    for (const auto& pane : layout->panes) {
        if (!pane) continue;
        auto& tab = pane->view;
        if (!Sortable(tab) || SortCurrent(cache, *pane, s.appPrefs.folder_sort_mode)) continue;
#ifdef PULSE_FOLDER_SIZES_TESTING
        ++cache.stats.sort_passes;
#endif
        struct Key { uint64_t bytes; bool known; };
        std::vector<Key> keys;
        keys.reserve(tab.snapshot->size());
        for (const auto& entry : *tab.snapshot) {
            if (!entry.is_dir) keys.push_back({entry.size, true});
            else {
                const auto path = entry.full_path.empty() ? tab.current_path + L"\\" + entry.name : entry.full_path;
                const auto value = s.folderSizes.Get(path);
                keys.push_back({value.bytes, value.has_value});
            }
        }
        const auto less = [&](size_t a, size_t b) {
            const auto& left = (*tab.snapshot)[a];
            const auto& right = (*tab.snapshot)[b];
            const bool left_folder = left.is_dir || (!left.link_target.empty() && left.link_target_is_dir);
            const bool right_folder = right.is_dir || (!right.link_target.empty() && right.link_target_is_dir);
            if (left_folder != right_folder && folders != app::FolderSortMode::Mixed)
                return app::EntryLess(left, right, ui::SortColumn::Name, tab.sort_direction, folders, tab.current_path);
            // Unknown totals stay last within their group in either direction.
            if (keys[a].known != keys[b].known) return keys[a].known;
            if (keys[a].bytes != keys[b].bytes)
                return tab.sort_direction == ui::SortDirection::Asc ? keys[a].bytes < keys[b].bytes : keys[a].bytes > keys[b].bytes;
            return app::EntryLess(left, right, ui::SortColumn::Name, tab.sort_direction,
                app::FolderSortMode::Mixed, tab.current_path);
        };
        std::vector<size_t> order(keys.size());
        std::iota(order.begin(), order.end(), size_t{0});
        if (std::is_sorted(order.begin(), order.end(), less)) {
            RememberSort(cache, *pane, s.appPrefs.folder_sort_mode);
            continue;
        }
        std::sort(order.begin(), order.end(), less);
        std::vector<int> remap(order.size());
        auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
        snapshot->reserve(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            remap[order[i]] = static_cast<int>(i);
            snapshot->push_back((*tab.snapshot)[order[i]]);
        }
        const auto map_index = [&](int index) {
            return index >= 0 && static_cast<size_t>(index) < remap.size() ? remap[static_cast<size_t>(index)] : -1;
        };
        const int focus = map_index(tab.selected_index);
        const int anchor = map_index(tab.selection_anchor);
        const bool all = tab.all_selected;
        std::unordered_set<int> selected;
        for (int index : tab.selected) if (const int mapped = map_index(index); mapped >= 0) selected.insert(mapped);
        tab.SetSnapshot(std::move(snapshot));
        RememberSort(cache, *pane, s.appPrefs.folder_sort_mode);
        tab.all_selected = all;
        tab.selected = std::move(selected);
        tab.selected_index = focus;
        tab.selection_anchor = anchor;
        ++tab.selection_revision;
        ++tab.view_generation;
        changed = true;
    }
    if (changed) s.hitTestVmValid = false;
    return changed;
}
#ifdef PULSE_FOLDER_SIZES_TESTING
FolderSizesUiStats FolderSizesUiStatsForTest(const AppState& s) {
    return s.folder_sizes_ui_cache ? s.folder_sizes_ui_cache->stats : FolderSizesUiStats{};
}
#endif
}
