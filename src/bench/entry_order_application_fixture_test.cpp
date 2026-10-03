#include "../app/app_internal.h"
#include "../common/localization.h"
#include <cstdio>
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
        tab.sort_column = column;
        tab.SetSnapshot(listing(true));
        tab.scroll_y = 0;
        s.scrollAnimating = true;
        QueueCreatedItemReveal(s, path + L"\\created");
        Check(tab.selected_index == 96 && tab.EntryAt(96).name == L"created" &&
            tab.pending_created_name.empty(), "non-time creation moves to the end and is selected");
        Check(tab.scroll_y > 0 && visible() && !s.scrollAnimating && s.scrollTargetY == tab.scroll_y,
            "non-time creation reveals the last row and cancels the old scroll target");
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
        "matching enumeration selects and reveals the created last row");
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
    tab.pending_generation = 42; result.generation = 42;
    ApplyWorkerResult(s, result);
    Check(tab.EntryAt(tab.selected_index).name == L"8.txt",
        "ordinary additions without a local creation intent do not steal selection");
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    const bool created = argc == 2 && std::string_view(argv[1]) == "--created";
    s.hwnd = created
        ? CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"creation fixture", WS_OVERLAPPEDWINDOW,
            -30000, -30000, 1100, 720, nullptr, nullptr, nullptr, nullptr)
        : CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!s.hwnd) { owned.reset(); OleUninitialize(); return 2; }
    if (created) {
        if (!s.compositor.Init(s.hwnd)) { DestroyWindow(s.hwnd); owned.reset(); OleUninitialize(); return 2; }
        s.renderer.SetCompositor(&s.compositor);
    }
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring path = L"\\\\?\\C:\\pulse-entry-order-memory-fixture";
    tab.current_path = path;
    tab.loading = false;
    tab.SetSnapshot(Listing({L"z.txt", L"a.txt"}));
    tab.order_held = true;
    tab.SelectOnly(0);
    s.store.Put(path, tab.snapshot);

    if (created) {
        CreatedItemCases(s, tab, path);
        s.watches.Stop();
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        owned.reset(); OleUninitialize();
        return failures ? 1 : 0;
    }

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
        tab.held_renames[1].new_name == L"final.txt" && tab.refresh_keeps_order,
        "ordinary rename chain preserves original rows and mapping before fallback enumeration");
    sorted.generation = 27;
    tab.pending_generation = 27;
    sorted.snapshot = Listing({L"a.txt", L"final.txt", L"z.txt"});
    ApplyWorkerResult(s, sorted);
    Check(tab.EntryAt(0).name == L"z.txt" && tab.EntryAt(1).name == L"final.txt" &&
        tab.EntryAt(2).name == L"a.txt" && tab.IsSelected(1),
        "ordinary rename chain follows the selected item in its original slot after fallback");

    tab.order_held = true;
    tab.sort_column = ui::SortColumn::Name; tab.sort_direction = ui::SortDirection::Asc;
    tab.search_relevance = false;
    SetSort(s, ui::SortColumn::Name, ui::SortDirection::Desc);
    Check(s.explicit_entry_refreshes.contains(&tab) && !tab.refresh_keeps_order,
        "switching sort enters explicit request rather than background hold");
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

    s.watches.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset(); OleUninitialize();
    return failures ? 1 : 0;
}
