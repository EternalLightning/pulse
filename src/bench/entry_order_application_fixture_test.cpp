#include "../app/app_internal.h"
#include "../app/app_column_view.h"
#include "../common/localization.h"
#include "../app/entry_sort.h"
#include "unc_search_banner_fixture.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string_view>

// Application-level request/result race fixture; worker/watch/services never
// start. All listings, notifications and store dirty markers are memory-only.
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
pulse::fs::SnapshotPtr Listing(std::initializer_list<const wchar_t*> names) {
    auto entries = std::make_shared<std::vector<pulse::fs::DirEntry>>();
    for (const auto* name : names) {
        pulse::fs::DirEntry entry;
        entry.name = name; entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entries->push_back(std::move(entry));
    }
    return entries;
}

void TransferAndTitleCases(pulse::AppState& s, pulse::app::Tab& tab, const std::wstring& path) {
    using namespace pulse;
    const auto title_is = [&](const std::wstring& expected) {
        wchar_t title[512]{};
        BuildVm(s, false);
        GetWindowTextW(s.hwnd, title, static_cast<int>(std::size(title)));
        return title == expected;
    };
    Check(title_is(app::LayoutTabTitle(*s.window_tabs.Active()) + L" - Pulse"),
        "taskbar title uses the active folder tab name");
    s.window_tabs.Active()->title = L"Pinned work";
    Check(title_is(L"Pinned work - Pulse"), "custom tab rename updates the taskbar title");
    s.window_tabs.NewTabAt(1, app::MakeHomePath());
    s.pane = s.window_tabs.Active()->FocusedPane();
    Check(title_is(app::LayoutTabTitle(*s.window_tabs.Active()) + L" - Pulse"),
        "switching tabs updates the taskbar title");
    s.window_tabs.SwitchTab(0);
    s.pane = s.window_tabs.Active()->FocusedPane();
    s.window_tabs.Active()->title.clear();
    tab.virtual_title = L"Search results";
    Check(title_is(L"Search results - Pulse"), "virtual tab title updates the taskbar title");
    tab.virtual_title.clear();

    for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
        tab.sort_column = ui::SortColumn::Name; tab.sort_direction = direction;
        tab.pending_generation = 0; tab.loading = false;
        tab.SetSnapshot(Listing({L"z.txt", L"m.txt"})); tab.SelectOnly(1);
        s.store.Put(path, tab.snapshot);
        QueueTransferredItemsReveal(s, {path + L"\\a.txt", path + L"\\b (2).txt",
            path + L"\\other\\nested.txt"});
        Check(tab.pending_created_names == std::vector<std::wstring>{L"a.txt", L"b (2).txt"},
            "transfer reveal uses actual conflict-renamed destinations and ignores nested files");
        tab.pending_generation = 60;
        app::WorkResult result{};
        result.path = path; result.generation = 59;
        result.snapshot = Listing({L"z.txt", L"m.txt", L"b (2).txt", L"a.txt"});
        ApplyWorkerResult(s, result);
        Check(!tab.pending_created_names.empty() && tab.selected_index == 1,
            "stale enumeration does not consume transferred selection");
        result.generation = 60;
        ApplyWorkerResult(s, result);
        Check(tab.pending_created_names.empty() && tab.SelectedCount() == 2 &&
            tab.EntryAt(tab.selected_index).name == L"a.txt" &&
            std::is_sorted(tab.snapshot->begin(), tab.snapshot->end(), [&](const auto& a, const auto& b) {
                return app::EntryLess(a, b, tab.sort_column, direction, path);
            }), "matching transfer enumeration sorts all rows and selects every transferred file");
    }
    tab.pending_generation = 0; tab.SelectOnly(0);
    QueueTransferredItemsReveal(s, {path + L"\\M.TXT"});
    Check(tab.pending_created_name == L"M.TXT", "replacement waits for refreshed metadata");
    tab.pending_generation = 61;
    app::WorkResult replacement{};
    replacement.path = path; replacement.generation = 61;
    replacement.snapshot = Listing({L"m.txt", L"z.txt"});
    ApplyWorkerResult(s, replacement);
    Check(tab.EntryAt(tab.selected_index).name == L"m.txt" && tab.pending_created_names.empty(),
        "replaced destination is selected by its actual filename casing after enumeration");
    const auto selected = tab.selected_index;
    QueueTransferredItemsReveal(s, {path + L"\\elsewhere\\a.txt"});
    Check(tab.pending_created_name.empty() && tab.selected_index == selected,
        "transfer to another folder does not steal current selection");
}

void CreatedItemCases(pulse::AppState& s, pulse::app::Tab& tab, const std::wstring& path) {
    using namespace pulse;
    const auto listing = [](bool include_new, bool folder = false) {
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        for (unsigned i = 0; i < 96; ++i) {
            fs::DirEntry entry;
            entry.name = std::to_wstring(i) + L".txt";
            entry.mtime.dwLowDateTime = 100 + i;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            entries->push_back(std::move(entry));
        }
        if (include_new) {
            fs::DirEntry entry;
            entry.name = L"created";
            entry.mtime.dwLowDateTime = 1000;
            entry.is_dir = folder;
            entry.attrs = folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            entries->insert(entries->begin() + 2, std::move(entry));
        }
        return entries;
    };
    const auto visible = [&] {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const auto bounds = FocusedPaneRect(s);
        const auto list = s.renderer.PaneListRect(pane, bounds);
        const auto item = s.renderer.ItemRectInPane(pane, bounds, pane.ViewIndex(tab.selected_index));
        return item.top >= list.top - 0.01f && item.bottom <= list.bottom + 0.01f;
    };
    for (const auto column : {ui::SortColumn::Name, ui::SortColumn::Size, ui::SortColumn::Type}) {
        for (const bool folder : {false, true}) {
            for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
                tab.sort_column = column;
                tab.sort_direction = direction;
                tab.SetSnapshot(listing(true, folder));
                tab.order_held = tab.refresh_keeps_order = true;
                tab.scroll_y = folder ? MaxScrollForActivePane(s) : 0;
                s.scrollAnimating = true;
                QueueCreatedItemReveal(s, path + L"\\created");
                const auto less = [&](const auto& left, const auto& right) {
                    return app::EntryLess(left, right, column, direction,
                        app::FolderSortModeFromInt(s.appPrefs.folder_sort_mode), path);
                };
                Check(tab.selected_index >= 0 && tab.EntryAt(tab.selected_index).name == L"created" &&
                    tab.pending_created_name.empty() && !tab.order_held && !tab.refresh_keeps_order &&
                    std::is_sorted(tab.snapshot->begin(), tab.snapshot->end(), less),
                    "creation sorts files and folders by the current column and direction and selects the item");
                Check(visible() && !s.scrollAnimating && s.scrollTargetY == tab.scroll_y,
                    "creation reveals its sorted row and cancels the old scroll target");
            }
        }
    }
    tab.sort_column = ui::SortColumn::Mtime;
    for (const bool folder : {false, true}) {
        for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            tab.sort_direction = direction;
            tab.SetSnapshot(listing(true, folder));
            tab.scroll_y = direction == ui::SortDirection::Asc ? 0 : MaxScrollForActivePane(s);
            QueueCreatedItemReveal(s, path + L"\\created");
            const int expected = direction == ui::SortDirection::Desc ? 0 : 96;
            Check(tab.selected_index == expected && tab.EntryAt(expected).name == L"created" &&
                tab.EntryAt(expected).is_dir == folder,
                "time-sorted creation follows direction for both files and folders");
            Check(visible(), "time-sorted creation is visible at its sorted position");
        }
    }
    tab.sort_column = ui::SortColumn::Name;
    tab.sort_direction = ui::SortDirection::Asc;
    tab.SetSnapshot(listing(false)); tab.SelectOnly(10); tab.scroll_y = 0;
    QueueCreatedItemReveal(s, path + L"\\created");
    Check(tab.pending_created_name == L"created" && tab.selected_index == 10,
        "completion before the listing arrives retains its reveal intent");
    RefreshPath(s, path, RefreshReason::OperationCompleted);
    tab.pending_generation = 40;
    app::WorkResult result{};
    result.path = path; result.generation = 39; result.snapshot = listing(true);
    ApplyWorkerResult(s, result);
    Check(tab.pending_created_name == L"created" && tab.selected_index == 10,
        "stale enumeration does not consume the creation intent");
    result.generation = 40;
    ApplyWorkerResult(s, result);
    Check(tab.pending_created_name.empty() && tab.selected_index == 96 && visible(),
        "matching enumeration selects and reveals the created sorted row");
    tab.sort_column = ui::SortColumn::Mtime; tab.sort_direction = ui::SortDirection::Desc;
    tab.SetSnapshot(listing(false)); tab.pending_generation = 0;
    QueueCreatedItemReveal(s, path + L"\\created");
    RefreshPath(s, path, RefreshReason::OperationCompleted);
    tab.pending_generation = 41; result.generation = 41;
    ApplyWorkerResult(s, result);
    Check(tab.pending_created_name.empty() && tab.selected_index == 0 && visible(),
        "time-sorted delayed enumeration reveals the created first row");
    tab.SetSnapshot(listing(true)); tab.SelectOnly(8);
    RefreshPath(s, path, RefreshReason::OperationCompleted);
    tab.pending_generation = 43;
    QueueCreatedItemReveal(s, path + L"\\created");
    Check(tab.pending_created_name == L"created" && tab.selected_index == 8,
        "creation already listed waits for an in-flight enumeration before revealing");
    result.generation = 43;
    ApplyWorkerResult(s, result);
    Check(tab.pending_created_name.empty() && tab.selected_index == 0 && visible(),
        "in-flight enumeration cannot overwrite the creation selection with its old selection");
    tab.filter_text = L"*.txt"; tab.SetSnapshot(listing(true));
    QueueCreatedItemReveal(s, path + L"\\created");
    Check(tab.filter_text.empty() && visible(), "creation hidden by a filter clears the filter to reveal it");
    tab.SetSnapshot(listing(false)); tab.SelectOnly(8);
    QueueCreatedItemReveal(s, path + L"\\other-folder\\created");
    Check(tab.pending_created_name.empty() && tab.selected_index == 8,
        "creation in another folder does not move selection in the current folder");
    RefreshPath(s, path, RefreshReason::OperationCompleted);
    tab.pending_generation = 44; result.generation = 44;
    ApplyWorkerResult(s, result);
    Check(tab.EntryAt(tab.selected_index).name == L"8.txt",
        "ordinary additions without a local creation intent do not steal selection");
    const auto output = std::filesystem::absolute(L"bench_data/created-item-reveal");
    std::filesystem::create_directories(output);
    for (const float scale : {1.0f, 1.5f}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<int>(760 * scale), static_cast<int>(540 * scale));
        for (const bool dark : {false, true}) {
            s.darkMode = dark;
            tab.sort_column = ui::SortColumn::Name;
            tab.sort_direction = ui::SortDirection::Desc;
            tab.SetSnapshot(listing(true, true));
            tab.scroll_y = MaxScrollForActivePane(s);
            QueueCreatedItemReveal(s, path + L"\\created");
            Check(tab.selected_index == 0 && visible(),
                "new folder is sorted above files and revealed in a narrow pane at both DPI scales");
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(BuildVm(s, false), D2D1::RectF(0, 0, 760 * scale, 540 * scale),
                ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            Check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "sorted creation frame renders");
            const auto image = output / (std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png"));
            Check(s.compositor.SaveSnapshot(image.c_str()), "sorted creation visual evidence saved");
        }
    }
    s.scale = 1;
    s.renderer.SetScale(1);
    s.compositor.RecreateTextFormats(1);
    s.compositor.Resize(1000, 700);
}

void ListingRaceCases(pulse::AppState& s, pulse::app::Tab& tab, const std::wstring& path) {
    using namespace pulse;
    tab.sort_column = ui::SortColumn::Name;
    tab.sort_direction = ui::SortDirection::Asc;
    tab.applied_generation = tab.pending_generation = 0;
    tab.loading = false;
    tab.ClearEntryOrderHold();
    tab.SetSnapshot(Listing({L"a.txt", L"z.txt"}));
    s.explicit_entry_refreshes.erase(&tab);
    s.store.Put(path, tab.snapshot);
    RefreshPath(s, path, RefreshReason::Explicit);
    tab.pending_generation = 10;
    s.explicit_entry_refreshes[&tab].generation = 10;
    // The ordinary production drain marks dirty without touching a pending tab.
    s.notify_queue.push_back({path, false, {{FILE_ACTION_MODIFIED, L"z.txt", L""}}});
    DrainDirNotifies(s);
    Check(s.store.IsDirty(path) && !tab.refresh_keeps_order,
        "watch event during explicit enumeration keeps sorted request intent");
    app::WorkResult sorted{};
    sorted.path = path; sorted.generation = 10; sorted.snapshot = Listing({L"a.txt", L"z.txt"});
    ApplyWorkerResult(s, sorted);
    Check(tab.EntryAt(0).name == L"a.txt" && !tab.order_held && !tab.refresh_keeps_order &&
        s.explicit_entry_refreshes.contains(&tab),
        "dirty followup after F5 does not re-hold the sorted enumeration");
    sorted.generation = 11;
    s.explicit_entry_refreshes[&tab].generation = 11;
    tab.pending_generation = 11;
    ApplyWorkerResult(s, sorted);
    Check(!tab.order_held && !tab.refresh_keeps_order && !s.explicit_entry_refreshes.contains(&tab),
        "clean matching followup clears only the completed explicit intent");

    tab.SetSnapshot(Listing({L"z.txt", L"old.txt"}));
    tab.SelectOnly(1);
    tab.HoldEntryRename(L"old.txt", L"new.txt");
    tab.order_held = true;
    RefreshPath(s, path, RefreshReason::Explicit);
    tab.pending_generation = 20;
    s.explicit_entry_refreshes[&tab].generation = 20;
    const auto before = tab.snapshot;
    fs::DirNotifyEvent renamed;
    renamed.action = FILE_ACTION_RENAMED_NEW_NAME; renamed.old_name = L"old.txt"; renamed.name = L"new.txt";
    Check(!ApplyNotifyToVisible(s, path, renamed) && tab.snapshot == before && tab.held_renames.size() == 1,
        "direct watch patch cannot prune rename mapping during explicit enumeration");
    sorted.generation = 20; sorted.snapshot = Listing({L"new.txt", L"z.txt"});
    ApplyWorkerResult(s, sorted);
    Check(tab.EntryAt(0).name == L"new.txt" && tab.IsSelected(0) && !tab.order_held && !tab.refresh_keeps_order,
        "matching sorted result follows renamed selection without re-holding rows");
    sorted.generation = 21; tab.pending_generation = 21;
    s.explicit_entry_refreshes[&tab].generation = 21;
    ApplyWorkerResult(s, sorted);
    Check(!s.explicit_entry_refreshes.contains(&tab), "rename race followup releases explicit intent");

    tab.SetSnapshot(Listing({L"old.txt", L"z.txt"}));
    tab.SelectOnly(0);
    RefreshPath(s, path, RefreshReason::Explicit);
    tab.pending_generation = 24; s.explicit_entry_refreshes[&tab].generation = 24;
    s.notify_queue.push_back({path, false, {renamed}});
    DrainDirNotifies(s);
    sorted.generation = 24; sorted.snapshot = Listing({L"old.txt", L"z.txt"});
    ApplyWorkerResult(s, sorted);
    Check(tab.held_renames.size() == 1 && !tab.refresh_keeps_order,
        "rename arriving after enumeration began survives its pre-rename result");
    sorted.generation = 25; sorted.snapshot = Listing({L"new.txt", L"z.txt"});
    tab.pending_generation = 25; s.explicit_entry_refreshes[&tab].generation = 25;
    ApplyWorkerResult(s, sorted);
    Check(tab.IsSelected(0) && tab.EntryAt(0).name == L"new.txt" && tab.held_renames.empty(),
        "explicit dirty retry restores renamed selection then clears consumed mapping");

    tab.SetSnapshot(Listing({L"z.txt", L"old.txt", L"a.txt"}));
    tab.SelectOnly(1);
    tab.order_held = true;
    tab.pending_generation = 0;
    s.store.Put(path, tab.snapshot);
    const auto before_chain = tab.snapshot;
    fs::DirNotifyEvent intermediate = renamed;
    intermediate.name = L"temporary.txt";
    fs::DirNotifyEvent final_name = renamed;
    final_name.old_name = intermediate.name;
    final_name.name = L"final.txt";
    s.notify_queue.push_back({path, false, {intermediate, final_name}});
    DrainDirNotifies(s);
    Check(tab.snapshot == before_chain && tab.held_renames.size() == 2 &&
        tab.held_renames[0].old_name == L"old.txt" &&
        tab.held_renames[0].new_name == L"temporary.txt" &&
        tab.held_renames[1].old_name == L"temporary.txt" &&
        tab.held_renames[1].new_name == L"final.txt" && !tab.refresh_keeps_order,
        "ordinary rename chain preserves original rows and mapping before fallback enumeration");
    sorted.generation = 27;
    tab.pending_generation = 27;
    sorted.snapshot = Listing({L"a.txt", L"final.txt", L"z.txt"});
    ApplyWorkerResult(s, sorted);
    Check(tab.EntryAt(0).name == L"a.txt" && tab.EntryAt(1).name == L"final.txt" &&
        tab.EntryAt(2).name == L"z.txt" && tab.IsSelected(1),
        "ordinary rename chain sorts the fallback result and follows the selected item");

    tab.order_held = true;
    tab.sort_column = ui::SortColumn::Name; tab.sort_direction = ui::SortDirection::Asc;
    tab.search_relevance = false;
    SetSort(s, ui::SortColumn::Name, ui::SortDirection::Desc);
    Check(tab.EntryAt(0).name == L"z.txt" && !tab.order_held && !tab.refresh_keeps_order &&
        tab.EntryAt(tab.selected_index).name == L"final.txt",
        "switching sort immediately sorts the snapshot and preserves selection");
    RefreshPath(s, path, RefreshReason::Explicit);
    tab.pending_generation = 30;
    s.explicit_entry_refreshes[&tab].generation = 30;
    app::WorkResult stale = sorted;
    stale.generation = 29;
    ApplyWorkerResult(s, stale);
    Check(tab.pending_generation == 30 && s.explicit_entry_refreshes[&tab].generation == 30,
        "stale completion does not clear newer explicit request");
    sorted.generation = 30; sorted.snapshot = Listing({L"z.txt", L"final.txt", L"a.txt"});
    ApplyWorkerResult(s, sorted);
    Check(!tab.order_held && tab.EntryAt(0).name == L"z.txt" && !s.explicit_entry_refreshes.contains(&tab),
        "matching sort completion retains sorted order and clears intent");
}

void ArchitectureCases(pulse::AppState& s, pulse::app::Tab& tab, const std::wstring& path) {
    using namespace pulse;
    s.themeOverride = ui::ThemeMode::Dark;
    s.appPrefs.theme_mode = 1;
    Check(s.EffectiveThemeMode() == ui::ThemeMode::Light,
        "explicit theme projects from preferences instead of stale startup override");
    s.appPrefs.theme_mode = 0;
    Check(s.EffectiveThemeMode() == ui::ThemeMode::Auto,
        "system theme preference projects without rewriting window fallback");
    s.appPrefs.theme_mode = -1;
    Check(s.EffectiveThemeMode() == ui::ThemeMode::Dark,
        "legacy session theme remains a startup fallback");
    s.shot.active = true; s.appPrefs.theme_mode = 1;
    Check(s.EffectiveThemeMode() == ui::ThemeMode::Dark,
        "shot appearance override remains independent of persisted preference");
    s.shot.active = false;

    auto second = std::make_unique<app::Pane>();
    second->view.current_path = path;
    auto& other = second->view;
    s.window_tabs.Active()->panes.push_back(std::move(second));
    app::RebuildLayoutRoot(*s.window_tabs.Active());
    tab.pending_generation = other.pending_generation = 0;
    tab.pending_created_name.clear();
    tab.SetSnapshot(Listing({L"a.txt", L"b.txt", L"c.txt"}));
    other.SetSnapshot(tab.snapshot);
    const auto shared_before = tab.snapshot;
    tab.SelectOnly(2); other.SelectOnly(0);
    s.store.Put(path, shared_before);
    tab.column_strip.child.path = path;
    tab.column_strip.child.snapshot = shared_before;
    tab.column_strip.child.requested = true;
    tab.column_strip.child.rows = std::make_shared<std::vector<int>>(std::initializer_list<int>{0, 1, 2});
    Check(ApplyNotifyToVisible(s, path, {FILE_ACTION_REMOVED, L"b.txt", {}}) &&
        tab.snapshot == other.snapshot && tab.snapshot == s.store.Peek(path) &&
        tab.snapshot != shared_before && tab.EntryCount() == 2 && shared_before->size() == 3,
        "watch patch shares one new immutable listing across identical panes and store");
    Check(tab.column_strip.child.snapshot == tab.snapshot && !tab.column_strip.child.rows &&
        tab.column_strip.child.requested && tab.column_strip.child.generation == 0,
        "watch publication updates column projection and invalidates its source-index rows");
    Check(tab.EntryAt(tab.selected_index).name == L"c.txt" &&
        other.EntryAt(other.selected_index).name == L"a.txt",
        "shared watch data preserves independent pane selection");
    other.SetSnapshot(Listing({L"c.txt", L"a.txt"}));
    s.store.Put(path, tab.snapshot);
    Check(ApplyNotifyToVisible(s, path, {FILE_ACTION_REMOVED, L"absent.txt", {}}) &&
        tab.EntryAt(0).name == L"a.txt" && other.EntryAt(0).name == L"a.txt" &&
        tab.snapshot != other.snapshot,
        "distinct pane snapshots are independently sorted on notification publication");
    const auto first_before_failure = tab.snapshot;
    const auto other_before_failure = other.snapshot;
    const auto store_before_failure = s.store.Peek(path);
    Check(!ApplyNotifyToVisible(s, path, {FILE_ACTION_RENAMED_OLD_NAME, L"a.txt", {}}) &&
        tab.snapshot == first_before_failure && other.snapshot == other_before_failure &&
        s.store.Peek(path) == store_before_failure && s.store.IsDirty(path),
        "unsupported watch patch publishes no pane or cache and marks shared cache dirty");
    Check(!tab.column_strip.child.requested && !tab.column_strip.child.rows,
        "failed watch patch also invalidates the column listing for revalidation");
    tab.column_layout = true;
    tab.column_strip.for_path = path;
    app::ColumnStripColumn pending_column;
    pending_column.path = path;
    pending_column.snapshot = tab.snapshot;
    tab.column_strip.ancestors.push_back(std::move(pending_column));
    tab.pending_generation = 91;
    SyncColumnStrips(s);
    Check(s.store.IsDirty(path) && tab.column_strip.ancestors.back().generation == 91,
        "column refresh shares pending generation without consuming its watch dirty marker");
    tab.pending_generation = 0;
    tab.column_layout = false;

    tab.current_path = L"pulse:changes:C:\\fixture";
    other.current_path = path;
    tab.loading = true; tab.change_request = 77;
    s.changes.details[77] = {&tab, tab.current_path, tab.view_generation, false};
    s.changes.summary_request = 88; s.changes.summary_pending = true;
    s.changes.popover.visible = true;
    s.changes.requested_paths = {L"C:\\fixture"};
    s.changes.summaries[L"fixture"] = {};
    s.appPrefs.change_tracking_enabled = false;
    ApplySettingsEffects(s, app::SettingsEffect::ChangeTracking);
    Check(s.changes.details.empty() && !s.changes.summary_pending &&
        s.changes.summary_request == 0 && s.changes.summaries.empty() &&
        s.changes.requested_paths.empty() && !s.changes.popover.visible && !tab.loading &&
        tab.change_request == 0,
        "disable setting invalidates detail and summary requests immediately without a UI tick");
    const auto disabled_listing = tab.snapshot;
    index::ChangeResponse response;
    response.state = index::ChangeState::Available;
    ApplyChangeDetails(s, 77, response);
    ApplyChangeSummaries(s, 88, response);
    Check(tab.snapshot == disabled_listing && s.changes.summaries.empty(),
        "disabled tracking ignores late detail and summary responses");
    s.appPrefs.change_tracking_enabled = true;
    s.appPrefs.change_tracking_days = 3;
    ApplySettingsEffects(s, app::SettingsEffect::ChangeTracking);
    Check(tab.change_days == 3 && tab.change_request != 0 &&
        s.changes.details.contains(tab.change_request),
        "enabled range setting starts a new detail request from the sole preference source");
    s.changes.summary_request = 90; s.changes.summary_days = 7;
    s.changes.requested_paths = {L"C:\\fixture"};
    index::ChangeSummary summary;
    summary.path = L"C:\\fixture";
    summary.state = index::ChangeState::Available;
    summary.count = 1;
    response.summaries.push_back(summary);
    ApplyChangeSummaries(s, 90, response);
    Check(s.changes.summaries.empty(), "summary response retains its own captured range validation");

}
} // namespace

int main(int argc, char** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Created item fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 1000, 700, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        std::puts("[FAIL] initialize isolated created-item compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        owned.reset(); OleUninitialize();
        return 2;
    }
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    const std::wstring path = fs::NormalizePath(L"C:\\pulse-created-item-memory");
    auto& tab = *ActiveTab(s);
    tab.current_path = path;
    const bool transfer_only = argc == 2 && std::string_view(argv[1]) == "--transfer-title-only";
    const bool listing_only = argc == 2 && std::string_view(argv[1]) == "--listing-races-only";
    const bool banner_only = argc == 2 && std::string_view(argv[1]) == "--unc-banner-only";
    if (transfer_only) TransferAndTitleCases(s, tab, path);
    else if (listing_only) ListingRaceCases(s, tab, path);
    else if (banner_only) failures += bench::RunUncSearchBannerCases(s, tab);
    else CreatedItemCases(s, tab, path);
    if (!banner_only && !transfer_only && !listing_only &&
        (argc != 2 || std::string_view(argv[1]) != "--created-only")) {
        ListingRaceCases(s, tab, path);
        ArchitectureCases(s, tab, path);
    }
    s.watches.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    return failures ? 1 : 0;
}
