#include "../app/app_model.h"
#include "../app/entry_order_hold.h"
#include "../app/entry_sort.h"
#include "../app/snapshot_patch.h"
#include "../common/known_folder_labels.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <shlwapi.h>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
} // namespace pulse::app

namespace {

using namespace pulse;

struct Fixture {
    std::filesystem::path temporary;
    std::filesystem::path path;
    bool valid = false;

    Fixture() {
        wchar_t buffer[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", buffer, MAX_PATH) &&
            !GetTempPathW(MAX_PATH, buffer)) {
            std::printf("[DIAG] GetTempPathW error=%lu\n", GetLastError());
            return;
        }
        temporary = buffer;
        path = temporary / (L"pulse-entry-order-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        valid = std::filesystem::create_directory(path, error);
        if (!valid) std::wprintf(L"[DIAG] fixture=%ls error=%d\n", path.c_str(), error.value());
    }
    ~Fixture() {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        const auto parent = std::filesystem::weakly_canonical(temporary, error);
        if (valid && !error && resolved.parent_path() == parent &&
            resolved.filename().wstring().starts_with(L"pulse-entry-order-"))
            std::filesystem::remove_all(resolved, error);
    }
    bool File(const wchar_t* name, const wchar_t* content = L"fixture") const {
        return WriteUtf8FileAtomic((path / name).wstring(), content);
    }
};

fs::DirEntry Entry(std::wstring name, uint64_t size = 0, bool folder = false) {
    fs::DirEntry entry;
    entry.name = std::move(name);
    entry.size = size;
    entry.is_dir = folder;
    entry.attrs = folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    return entry;
}

bool NamesAre(const std::vector<fs::DirEntry>& entries,
              std::initializer_list<const wchar_t*> expected) {
    if (entries.size() != expected.size()) return false;
    size_t index = 0;
    for (const auto* name : expected)
        if (entries[index++].name != name) return false;
    return true;
}

std::vector<fs::DirEntry> Sorted(std::vector<fs::DirEntry> entries,
                               ui::SortColumn column, ui::SortDirection direction,
                               const std::wstring& parent) {
    std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
        return app::EntryLess(a, b, column, direction, parent);
    });
    return entries;
}

void ApplySnapshot(app::Tab& tab, std::vector<fs::DirEntry> entries) {
    std::vector<std::wstring> names;
    for (int index : tab.SelectedIndices()) names.push_back(tab.EntryAt(index).name);
    std::wstring focus = tab.selected_index < 0 ? L"" : tab.EntryAt(tab.selected_index).name;
    auto fresh = std::make_shared<std::vector<fs::DirEntry>>(std::move(entries));
    tab.SetSnapshot(tab.PrepareEntrySnapshot(fresh, names, focus));
    if (!names.empty()) tab.RemapSelection(names, focus);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    Fixture fixture;
    if (!fixture.valid) {
        std::printf("[FAIL] create isolated entry-order fixture\n");
        return 1;
    }
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    const std::wstring parent = fixture.path.wstring();
    if (argc == 2 && wcscmp(argv[1], L"--directory-contracts-only") == 0) {
        const std::vector<fs::DirEntry> facts{Entry(L"z.txt", 1), Entry(L"a.txt", 10),
                                            Entry(L"m-folder", 0, true)};
        const auto shared = std::make_shared<std::vector<fs::DirEntry>>(facts);
        app::Tab name_tab, size_tab;
        name_tab.current_path = parent; size_tab.current_path = parent;
        size_tab.sort_column = ui::SortColumn::Size;
        size_tab.sort_direction = ui::SortDirection::Desc;
        std::vector<std::wstring> names;
        std::wstring focus;
        name_tab.SetSnapshot(name_tab.PrepareEntrySnapshot(shared, names, focus));
        size_tab.SetSnapshot(size_tab.PrepareEntrySnapshot(shared, names, focus));
        check(NamesAre(*name_tab.snapshot, {L"m-folder", L"a.txt", L"z.txt"}) &&
              NamesAre(*size_tab.snapshot, {L"m-folder", L"a.txt", L"z.txt"}) &&
              NamesAre(*shared, {L"z.txt", L"a.txt", L"m-folder"}),
              "shared facts are projected by each pane without mutating the shared listing");
        size_tab.sort_direction = ui::SortDirection::Asc;
        size_tab.PrepareEntryRefresh(true);
        ApplySnapshot(size_tab, facts);
        check(NamesAre(*size_tab.snapshot, {L"m-folder", L"z.txt", L"a.txt"}),
              "opposite size order does not inherit another pane's name order");
        name_tab.SelectOnly(1);
        name_tab.PrepareEntryRefresh(false);
        ApplySnapshot(name_tab, {Entry(L"z.txt", 20), Entry(L"a.txt", 1),
                                Entry(L"m-folder", 0, true), Entry(L"0-new.txt", 7)});
        check(!name_tab.order_held && NamesAre(*name_tab.snapshot,
              {L"m-folder", L"0-new.txt", L"a.txt", L"z.txt"}) &&
              name_tab.EntryAt(name_tab.selected_index).name == L"a.txt",
              "background projection sorts newcomers and retains selection across foreign ordering");
        name_tab.PrepareEntryRefresh(true);
        ApplySnapshot(name_tab, *name_tab.snapshot);
        check(!name_tab.order_held && NamesAre(*name_tab.snapshot,
              {L"m-folder", L"0-new.txt", L"a.txt", L"z.txt"}) &&
              name_tab.EntryAt(name_tab.selected_index).name == L"a.txt",
              "explicit projection restores sorting while retaining selection");
        for (const auto folders : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::Mixed}) {
            app::Tab tab;
            tab.current_path = parent;
            tab.SetSnapshot(tab.PrepareEntrySnapshot(shared, names, focus, folders));
            check(tab.EntryAt(0).name == (folders == app::FolderSortMode::Mixed ? L"a.txt" : L"m-folder"),
                  "folder mode is part of each pane's projection identity");
        }
        const std::vector<fs::DirEntry> shown{Entry(L"unavailable.txt", 42), Entry(L"keep.txt", 7)};
        for (const bool held : {false, true}) {
            auto rows = shown;
            check(app::ApplyDirNotify(rows, parent,
                  {FILE_ACTION_MODIFIED, L"unavailable.txt", {}}, ui::SortColumn::Name,
                  ui::SortDirection::Asc, held) == app::NotifyPatch::NeedFullEnum &&
                  NamesAre(rows, {L"unavailable.txt", L"keep.txt"}) && rows[0].size == 42,
                  "failed stat preserves old row and requests full enumeration");
            rows = shown;
            check(app::ApplyDirNotify(rows, parent,
                  {FILE_ACTION_ADDED, L"new-unavailable.txt", {}}, ui::SortColumn::Name,
                  ui::SortDirection::Asc, held) == app::NotifyPatch::NeedFullEnum &&
                  NamesAre(rows, {L"unavailable.txt", L"keep.txt"}),
                  "unavailable added entry defers without changing existing rows");
            std::vector<fs::DirNotifyEvent> burst(5, {FILE_ACTION_MODIFIED, L"unavailable.txt", {}});
            rows = shown;
            check(app::ApplyDirNotifyBatch(rows, parent, burst, ui::SortColumn::Name,
                  ui::SortDirection::Asc, held) == app::NotifyPatch::NeedFullEnum &&
                  NamesAre(rows, {L"unavailable.txt", L"keep.txt"}),
                  "failed batch stat leaves the original listing intact");
            check(app::ApplyDirNotify(rows, parent,
                  {FILE_ACTION_REMOVED, L"unavailable.txt", {}}, ui::SortColumn::Name,
                  ui::SortDirection::Asc, held) == app::NotifyPatch::Applied &&
                  NamesAre(rows, {L"keep.txt"}), "explicit removal still deletes the row");
        }
        return failures ? 1 : 0;
    }
    const std::vector<fs::DirEntry> shown{ Entry(L"b.txt", 1), Entry(L"m.txt", 2),
                                           Entry(L"z.txt", 3) };
    const std::vector<app::EntryRenameHint> renames{{L"m.txt", L"temporary.txt"},
                                                  {L"temporary.txt", L"a.txt"}};
    auto fresh = Sorted({Entry(L"a.txt", 20), Entry(L"b.txt", 10), Entry(L"z.txt", 30),
                         Entry(L"0-new.txt", 4), Entry(L"new-folder", 0, true)},
                        ui::SortColumn::Name, ui::SortDirection::Asc, parent);
    const auto held = app::KeepEntryOrder(shown, fresh, renames,
                                          ui::SortColumn::Name, ui::SortDirection::Asc, parent);
    check(NamesAre(held, {L"b.txt", L"a.txt", L"z.txt", L"new-folder", L"0-new.txt"}) &&
          held[0].size == 10 && held[1].size == 20,
          "rename chains retain physical rows, update metadata and append sorted newcomers");
    const std::vector<fs::DirEntry> newcomers{Entry(L"z-folder", 0, true), Entry(L"a.txt")};
    check(NamesAre(app::KeepEntryOrder({}, newcomers, {}, ui::SortColumn::Name,
              ui::SortDirection::Asc, parent, app::FolderSortMode::Mixed), {L"a.txt", L"z-folder"}),
          "held refresh uses captured mixed-folder preference for new rows");
    check(NamesAre(app::KeepEntryOrder({}, newcomers, {}, ui::SortColumn::Name,
              ui::SortDirection::Desc, parent, app::FolderSortMode::FollowDirection), {L"a.txt", L"z-folder"}),
          "held refresh uses captured direction-following folder preference");
    check(NamesAre(app::KeepEntryOrder({}, newcomers, {}, ui::SortColumn::Name,
              ui::SortDirection::Desc, parent, app::FolderSortMode::FoldersFirst), {L"z-folder", L"a.txt"}),
          "folder preference is an explicit immutable sort input rather than global state");
    const auto without_removed = app::KeepEntryOrder(shown,
        {Entry(L"b.txt"), Entry(L"new.txt")}, {}, ui::SortColumn::Size,
        ui::SortDirection::Desc, parent);
    check(NamesAre(without_removed, {L"b.txt", L"new.txt"}),
          "full enumeration drops missing rows without leaving held slots");
    std::vector<std::wstring> names{L"m.txt", L"b.txt"};
    std::wstring focus = L"m.txt";
    app::FollowEntryRenames(renames, fresh, names, focus);
    check(names == std::vector<std::wstring>{L"a.txt", L"b.txt"} && focus == L"a.txt",
          "multiselection and focus follow actual renamed leaf names");
    auto pending_renames = renames;
    app::PruneEntryRenames(pending_renames, fresh);
    check(pending_renames.empty(), "completed rename chains are pruned");
    pending_renames = renames;
    app::PruneEntryRenames(pending_renames, shown);
    names = { L"m.txt" };
    focus = L"m.txt";
    app::FollowEntryRenames(pending_renames, fresh, names, focus);
    check(pending_renames.size() == 2 && names[0] == L"a.txt" && focus == L"a.txt",
          "an early refresh keeps unresolved intermediate rename hints for the later result");
    pending_renames = {{L"b.txt", L"failed.txt"}, {L"gone.txt", L"also-gone.txt"}};
    app::PruneEntryRenames(pending_renames, fresh);
    check(pending_renames.size() == 1 && pending_renames[0].old_name == L"b.txt",
          "deleted rename sources are removed while unapplied operations remain pending");
    const auto virtual_entries = app::KeepEntryOrder(shown, fresh, renames,
        ui::SortColumn::Name, ui::SortDirection::Asc, app::MakeStarredPath());
    check(NamesAre(virtual_entries, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}),
          "virtual results retain the supplied sorted listing");

    app::Tab tab;
    tab.current_path = parent;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(shown));
    tab.SelectOnly(1);
    tab.HoldEntryRename(L"m.txt", L"a.txt");
    tab.PrepareEntryRefresh(false);
    ApplySnapshot(tab, Sorted({Entry(L"a.txt"), Entry(L"b.txt"), Entry(L"z.txt")},
        ui::SortColumn::Name, ui::SortDirection::Asc, parent));
    check(!tab.order_held && NamesAre(*tab.snapshot, {L"a.txt", L"b.txt", L"z.txt"}) &&
          tab.selected_index == 0 && tab.EntryAt(0).name == L"a.txt" && tab.held_renames.empty(),
          "a refresh arriving before directory notification sorts and follows the renamed selection");
    tab.PrepareEntryRefresh(false);
    ApplySnapshot(tab, fresh);
    check(NamesAre(*tab.snapshot, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}) &&
          tab.EntryAt(tab.selected_index).name == L"a.txt",
          "background full enumeration sorts new files and folders and preserves selection");
    tab.PrepareEntryRefresh(true);
    check(!tab.refresh_keeps_order && tab.pending_ensure_selection_visible,
          "explicit refresh requests sorted rows and reveals the retained physical selection");
    ApplySnapshot(tab, fresh);
    check(!tab.order_held && tab.EntryAt(tab.selected_index).name == L"a.txt" &&
          NamesAre(*tab.snapshot, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}),
          "F5 restores sort order without losing the renamed selection");
    tab.sort_column = ui::SortColumn::Size;
    tab.sort_direction = ui::SortDirection::Desc;
    tab.PrepareEntryRefresh(true);
    ApplySnapshot(tab, Sorted(fresh, tab.sort_column, tab.sort_direction, parent));
    check(!tab.order_held && tab.EntryAt(tab.selected_index).name == L"a.txt",
          "changing sort also restores sorted order while preserving selection");
    tab.pending_generation = 10;
    tab.PrepareEntryRefresh(false);
    check(!tab.refresh_keeps_order, "background notifications do not override an explicit in-flight load");
    tab.pending_generation = 0;
    tab.HoldEntryRename(L"a.txt", L"again.txt");
    tab.order_held = true;
    tab.PrepareEntryRefresh(false);
    tab.NavigateTo(parent);
    check(!tab.order_held && !tab.refresh_keeps_order && tab.held_renames.empty(),
          "reopening the same directory clears held order and rename hints");
    tab.current_path = app::MakeHomePath();
    tab.HoldEntryRename(L"a.txt", L"virtual.txt");
    tab.PrepareEntryRefresh(false);
    check(!tab.refresh_keeps_order && tab.held_renames.empty(),
          "virtual tab refreshes cannot hold physical rows");

    check(fixture.File(L"a.txt", L"new metadata") && fixture.File(L"c.txt", L"changed") &&
          fixture.File(L"x.txt") && fixture.File(L"0.txt") &&
          CreateDirectoryW((fixture.path / L"folder").c_str(), nullptr),
          "create notification fixture files and folder");
    const std::vector<fs::DirEntry> before{Entry(L"d.txt"), Entry(L"b.txt"),
                                         Entry(L"c.txt"), Entry(L"folder", 0, true)};
    const std::vector<fs::DirNotifyEvent> events{
        {FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"b.txt"},
        {FILE_ACTION_MODIFIED, L"c.txt", {}},
        {FILE_ACTION_ADDED, L"x.txt", {}},
        {FILE_ACTION_REMOVED, L"d.txt", {}},
        {FILE_ACTION_MODIFIED, L"a.txt", {}},
        {FILE_ACTION_ADDED, L"x.txt", {}},
        {FILE_ACTION_REMOVED, L"c.txt", {}},
        {FILE_ACTION_ADDED, L"c.txt", {}}};
    for (const auto column : {ui::SortColumn::Name, ui::SortColumn::Size,
                              ui::SortColumn::Mtime, ui::SortColumn::Type}) {
        for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            auto sequential = before;
            auto batch = before;
            bool applied = true;
            for (const auto& event : events)
                applied &= app::ApplyDirNotify(sequential, parent, event, column, direction, true) ==
                           app::NotifyPatch::Applied;
            applied &= app::ApplyDirNotifyBatch(batch, parent, events, column, direction, true) ==
                       app::NotifyPatch::Applied;
            bool identical = batch.size() == sequential.size();
            for (size_t index = 0; identical && index < batch.size(); ++index)
                identical = batch[index].name == sequential[index].name &&
                            batch[index].size == sequential[index].size;
            check(applied && identical && NamesAre(batch, {L"a.txt", L"folder", L"x.txt", L"c.txt"}),
                  "single and batched notifications agree on rename, update, append, delete and recreation");
        }
    }
    auto duplicates = std::vector<fs::DirEntry>{Entry(L"d.txt"), Entry(L"b.txt"),
                                               Entry(L"a.txt"), Entry(L"c.txt")};
    check(app::ApplyDirNotify(duplicates, parent, events[0], ui::SortColumn::Name,
          ui::SortDirection::Asc, true) == app::NotifyPatch::Applied &&
          NamesAre(duplicates, {L"d.txt", L"a.txt", L"c.txt"}),
          "rename collision removes the duplicate while retaining the renamed row's slot");
    const std::vector<fs::DirNotifyEvent> vanished_intermediate{
        {FILE_ACTION_RENAMED_NEW_NAME, L"temporary.txt", L"b.txt"},
        {FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"temporary.txt"}};
    auto rename_batch = before;
    auto rename_single = before;
    check(app::ApplyDirNotifyBatch(rename_batch, parent, vanished_intermediate,
          ui::SortColumn::Name, ui::SortDirection::Asc, true) == app::NotifyPatch::NeedFullEnum &&
          app::ApplyDirNotify(rename_single, parent, vanished_intermediate.front(),
          ui::SortColumn::Name, ui::SortDirection::Asc, true) == app::NotifyPatch::NeedFullEnum &&
          NamesAre(rename_batch, {L"d.txt", L"b.txt", L"c.txt", L"folder"}) &&
          NamesAre(rename_single, {L"d.txt", L"b.txt", L"c.txt", L"folder"}),
          "vanished rename intermediates defer atomically without deleting the held source slot");
    const std::vector<app::EntryRenameHint> queued_renames{{L"b.txt", L"temporary.txt"},
                                                          {L"temporary.txt", L"a.txt"}};
    const auto after_rename_enum = app::KeepEntryOrder(rename_batch,
        {Entry(L"a.txt"), Entry(L"c.txt"), Entry(L"d.txt"), Entry(L"folder", 0, true)},
        queued_renames, ui::SortColumn::Name, ui::SortDirection::Asc, parent);
    names = {L"b.txt"};
    focus = L"b.txt";
    app::FollowEntryRenames(queued_renames, after_rename_enum, names, focus);
    check(NamesAre(after_rename_enum, {L"d.txt", L"a.txt", L"c.txt", L"folder"}) &&
          names[0] == L"a.txt" && focus == L"a.txt",
          "fallback enumeration restores the final rename in its physical source slot and selection");
    auto missing = before;
    check(app::ApplyDirNotify(missing, parent, {FILE_ACTION_MODIFIED, L"b.txt", {}},
          ui::SortColumn::Name, ui::SortDirection::Asc, true) == app::NotifyPatch::NeedFullEnum &&
          NamesAre(missing, {L"d.txt", L"b.txt", L"c.txt", L"folder"}),
          "a failed touched-file stat retains its held row pending full enumeration");
    auto invalid = before;
    check(app::ApplyDirNotifyBatch(invalid, parent,
          {{FILE_ACTION_ADDED, L"child\\invalid.txt", {}}}, ui::SortColumn::Name,
          ui::SortDirection::Asc, true) == app::NotifyPatch::NeedFullEnum &&
          NamesAre(invalid, {L"d.txt", L"b.txt", L"c.txt", L"folder"}),
          "invalid notification paths defer to full enumeration without corrupting rows");

    check(fixture.File(L"resolved.txt", L"facts"), "create fact-only notification fixture");
    const auto resolved = app::ResolveDirNotifies(parent, {{FILE_ACTION_ADDED, L"resolved.txt", {}}});
    const bool removed_after_resolve = DeleteFileW((fixture.path / L"resolved.txt").c_str()) != FALSE;
    auto fact_rows = before;
    check(removed_after_resolve && resolved.status == app::NotifyPatch::Applied &&
          app::ApplyResolvedDirNotifies(fact_rows, resolved) == app::NotifyPatch::Applied &&
          fact_rows.back().name == L"resolved.txt" && fact_rows.back().size == 5,
          "applying resolved facts performs no new stat after the source file disappears");

    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto saved_labels = path::KnownFolderLabels();
    auto& labels = path::KnownFolderLabels();
    labels[1].path = parent + L"\\Z redirected";
    labels[2].path = parent + L"\\A redirected";
    fs::DirEntry downloads = Entry(L"Z redirected", 0, true);
    fs::DirEntry pictures = Entry(L"A redirected", 0, true);
    const bool localized_first = StrCmpLogicalW(l10n::Get(l10n::StringId::Downloads).c_str(),
        l10n::Get(l10n::StringId::KnownFolderPictures).c_str()) < 0;
    const auto localized = app::KeepEntryOrder({}, {pictures, downloads}, {},
        ui::SortColumn::Name, ui::SortDirection::Asc, parent);
    check(localized.size() == 2 && localized.front().name ==
          (localized_first ? downloads.name : pictures.name),
          "appended entries use the existing localized EntryLess with the physical parent");
    auto localized_tab = app::Tab{};
    localized_tab.current_path = parent;
    localized_tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(
        std::vector<fs::DirEntry>{downloads, pictures}));
    localized_tab.SelectOnly(0);
    localized_tab.PrepareEntryRefresh(true);
    ApplySnapshot(localized_tab, Sorted({downloads, pictures}, ui::SortColumn::Name,
                                       ui::SortDirection::Asc, parent));
    check(localized_tab.EntryAt(localized_tab.selected_index).name == downloads.name,
          "localized refresh remaps selection by physical name instead of display label");
    path::KnownFolderLabels() = saved_labels;
    std::printf("Entry-order hold failures: %d\n", failures);
    return failures ? 1 : 0;
}
