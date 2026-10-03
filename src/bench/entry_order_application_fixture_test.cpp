#include "../app/app_internal.h"
#include "../common/localization.h"
#include <cstdio>

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
}
int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!s.hwnd) { owned.reset(); OleUninitialize(); return 2; }
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring path = L"\\\\?\\C:\\pulse-entry-order-memory-fixture";
    tab.current_path = path;
    tab.loading = false;
    tab.SetSnapshot(Listing({L"z.txt", L"a.txt"}));
    tab.order_held = true;
    tab.SelectOnly(0);
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
