#include "../app/app_model.h"
#include "../app/entry_order_hold.h"
#include "../app/entry_sort.h"
#include "../app/directory_projection.h"
#include "../app/snapshot_patch.h"
#include "../common/known_folder_labels.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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
    pulse::fs::SnapshotPtr fresh = std::make_shared<std::vector<fs::DirEntry>>(std::move(entries));
    fresh = pulse::app::ProjectDirectorySnapshot(fresh, tab.current_path, tab.sort_column,
        tab.sort_direction, pulse::app::FolderSortMode::FoldersFirst);
    tab.SetSnapshot(tab.PrepareEntrySnapshot(fresh, names, focus));
    if (!names.empty()) tab.RemapSelection(names, focus);
}

int TestCaseSensitiveNames(Fixture& fixture) {
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    const auto resolved = [](std::vector<fs::DirNotifyEvent> events,
                             std::vector<fs::DirEntry> facts) {
        return app::ResolvedDirNotifies{app::NotifyPatch::Applied, std::move(events),
                                       std::move(facts)};
    };
    const std::vector<fs::DirEntry> original{Entry(L"A.txt", 1), Entry(L"a.txt", 2),
                                           Entry(L"keep.txt", 3)};
    for (const auto* name : {L"A.txt", L"a.txt"}) {
        auto rows = original;
        const auto survivor = std::wstring(name) == L"A.txt" ? L"a.txt" : L"A.txt";
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(fixture.path.wstring(), {{FILE_ACTION_REMOVED, name, {}}})) == app::NotifyPatch::Applied &&
              NamesAre(rows, {survivor, L"keep.txt"}),
              "single removal addresses the exact physical name");
        rows = original;
        check(app::ApplyResolvedDirNotifies(rows,
              resolved({{FILE_ACTION_REMOVED, name, {}}}, {Entry(L"")})) ==
              app::NotifyPatch::Applied && NamesAre(rows, {survivor, L"keep.txt"}),
              "resolved removal keeps the differently cased sibling");
        rows = original;
        check(app::ApplyResolvedDirNotifies(rows,
              resolved({{FILE_ACTION_MODIFIED, name, {}}}, {Entry(name, 20)})) ==
              app::NotifyPatch::Applied && NamesAre(rows, {L"A.txt", L"a.txt", L"keep.txt"}) &&
              rows[std::wstring(name) == L"A.txt" ? 0 : 1].size == 20 &&
              rows[std::wstring(name) == L"A.txt" ? 1 : 0].size ==
                  (std::wstring(name) == L"A.txt" ? 2 : 1),
              "resolved modification updates only its exact row");
    }
    auto rows = std::vector<fs::DirEntry>{Entry(L"a.txt", 2), Entry(L"keep.txt", 3)};
    check(app::ApplyResolvedDirNotifies(rows,
          resolved({{FILE_ACTION_ADDED, L"A.txt", {}}}, {Entry(L"A.txt", 1)})) ==
          app::NotifyPatch::Applied && NamesAre(rows, {L"a.txt", L"keep.txt", L"A.txt"}),
          "resolved addition appends a distinct case sibling");
    rows = original;
    check(app::ApplyResolvedDirNotifies(rows,
          resolved({{FILE_ACTION_RENAMED_NEW_NAME, L"B.txt", L"A.txt"}},
                   {Entry(L"B.txt", 10)})) == app::NotifyPatch::Applied &&
          NamesAre(rows, {L"B.txt", L"a.txt", L"keep.txt"}) && rows[1].size == 2,
          "resolved rename preserves the source slot and the case sibling");
    rows = original;
    check(app::ApplyResolvedDirNotifies(rows,
          resolved({{FILE_ACTION_RENAMED_NEW_NAME, L"A.txt", L"a.txt"}},
                   {Entry(L"A.txt", 10)})) == app::NotifyPatch::Applied &&
          NamesAre(rows, {L"A.txt", L"keep.txt"}) && rows[0].size == 10,
          "rename replacement removes only the exact destination duplicate");
    rows = {Entry(L"A.txt", 1), Entry(L"keep.txt", 3)};
    check(app::ApplyResolvedDirNotifies(rows,
          resolved({{FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"A.txt"}},
                   {Entry(L"a.txt", 10)})) == app::NotifyPatch::Applied &&
          NamesAre(rows, {L"a.txt", L"keep.txt"}),
          "case-only rename keeps its row when the destination is absent");
    rows = original;
    check(app::ApplyResolvedDirNotifies(rows,
          resolved({{FILE_ACTION_REMOVED, L"A.txt", {}},
                    {FILE_ACTION_MODIFIED, L"a.txt", {}},
                    {FILE_ACTION_ADDED, L"A.txt", {}},
                    {FILE_ACTION_REMOVED, L"a.txt", {}}},
                   {Entry(L""), Entry(L"a.txt", 20), Entry(L"A.txt", 30), Entry(L"")})) ==
          app::NotifyPatch::Applied && NamesAre(rows, {L"keep.txt", L"A.txt"}) &&
          rows.back().size == 30,
          "ordered add-remove burst maintains independent case identities");
    for (const auto* name : {L"A.txt", L"a.txt"}) {
        rows = original;
        std::vector<fs::DirNotifyEvent> burst(5, {FILE_ACTION_REMOVED, name, {}});
        const auto survivor = std::wstring(name) == L"A.txt" ? L"a.txt" : L"A.txt";
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(fixture.path.wstring(), burst)) == app::NotifyPatch::Applied &&
              NamesAre(rows, {survivor, L"keep.txt"}),
              "large resolved removal burst preserves the untouched case sibling");
    }
    std::vector<std::wstring> selected{L"A.txt", L"a.txt"};
    std::wstring focus = L"a.txt";
    app::FollowEntryRenames({{L"A.txt", L"B.txt"}},
                           {Entry(L"a.txt", 2), Entry(L"B.txt", 1)}, selected, focus);
    check(selected == std::vector<std::wstring>({L"B.txt", L"a.txt"}) && focus == L"a.txt",
          "selection rename follows only the exact source while sibling focus survives");
    app::Tab tab;
    tab.current_path = fixture.path.wstring();
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(
        std::vector<fs::DirEntry>{Entry(L"a.txt", 2), Entry(L"B.txt", 1)}));
    tab.RemapSelection(selected, focus);
    check(tab.SelectedCount() == 2 && tab.selected_index == 0,
          "selection remapping keeps both exact identities and the intended focus");
    selected = {L"A.txt"}; focus = L"A.txt";
    app::FollowEntryRenames({{L"A.txt", L"a.txt"}}, {Entry(L"a.txt", 1)}, selected, focus);
    check(selected[0] == L"a.txt" && focus == L"a.txt",
          "case-only rename updates selection and focus to the physical destination");
    std::vector<app::EntryRenameHint> hints{{L"A.txt", L"B.txt"}, {L"a.txt", L"c.txt"}};
    app::PruneEntryRenames(hints, {Entry(L"a.txt", 2)});
    check(hints.size() == 1 && hints[0].old_name == L"a.txt",
          "rename pruning does not retain hints for a differently cased source");

    hints = {{L"A.txt", L"a.txt"}};
    app::PruneEntryRenames(hints, {Entry(L"A.txt", 1)});
    check(hints.size() == 1 && hints[0].old_name == L"A.txt" && hints[0].new_name == L"a.txt",
          "an early refresh retains an unapplied case-only rename hint");
    app::PruneEntryRenames(hints, {Entry(L"a.txt", 1)});
    check(hints.empty(), "completed case-only rename hints are pruned by exact names");
    check(fixture.File(L"Ordinary.txt", L"normal"), "create ordinary-directory case rename fixture");
    check(MoveFileW((fixture.path / L"Ordinary.txt").c_str(),
                   (fixture.path / L"ordinary.txt").c_str()) != FALSE,
          "perform real case-only rename in an ordinary directory");
    rows = {Entry(L"Ordinary.txt", 1), Entry(L"keep.txt", 3)};
    check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(fixture.path.wstring(), {{FILE_ACTION_RENAMED_NEW_NAME, L"ordinary.txt", L"Ordinary.txt"}})) ==
          app::NotifyPatch::Applied && rows.size() == 2 &&
          std::count_if(rows.begin(), rows.end(), [](const auto& entry) {
              return entry.name == L"ordinary.txt" && entry.size == 6;
          }) == 1 && std::none_of(rows.begin(), rows.end(), [](const auto& entry) {
              return entry.name == L"Ordinary.txt";
          }), "ordinary case-only rename replaces the exact old name once");
    const auto case_path = fixture.path / L"case-sensitive";
    std::error_code error;
    if (!std::filesystem::create_directory(case_path, error)) {
        check(false, "create isolated case-sensitive directory");
        return failures ? 1 : 0;
    }
    HANDLE directory = CreateFileW(case_path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = directory != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &info, sizeof(info));
    const DWORD enable_error = enabled ? ERROR_SUCCESS : GetLastError();
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (!enabled) {
        std::printf("[SKIP] real case-sensitive directory unavailable error=%lu; model cases retained\n",
                    enable_error);
        return failures ? 1 : 0;
    }
    const auto write = [&](const wchar_t* name, const char* contents, DWORD creation) {
        HANDLE file = CreateFileW((case_path / name).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const DWORD length = static_cast<DWORD>(strlen(contents));
        DWORD written = 0;
        const bool written_all = WriteFile(file, contents, length, &written, nullptr) &&
                                 written == length;
        CloseHandle(file);
        return written_all;
    };
    check(write(L"A.txt", "U", CREATE_NEW) && write(L"a.txt", "lower", CREATE_NEW),
          "real case-sensitive directory contains both A.txt and a.txt");
    const auto size_of = [](const std::vector<fs::DirEntry>& entries, const wchar_t* name) {
        for (const auto& entry : entries) if (entry.name == name) return entry.size;
        return UINT64_MAX;
    };
    std::vector<fs::DirEntry> disk;
    check(fs::EnumerateDirectory(case_path.wstring(), disk) && disk.size() == 2 &&
          size_of(disk, L"A.txt") == 1 && size_of(disk, L"a.txt") == 5,
          "real enumeration returns both physical names and their independent sizes");
    {
        auto live = Sorted(disk, ui::SortColumn::Name, ui::SortDirection::Asc,
                           case_path.wstring());
        check(app::ApplyResolvedDirNotifies(live, app::ResolveDirNotifies(case_path.wstring(), {{FILE_ACTION_MODIFIED, L"a.txt", {}}})) == app::NotifyPatch::Applied &&
              live.size() == 2 && size_of(live, L"A.txt") == 1 && size_of(live, L"a.txt") == 5,
              "real single modification preserves both case siblings");
        std::vector<fs::DirNotifyEvent> burst{{FILE_ACTION_MODIFIED, L"A.txt", {}},
            {FILE_ACTION_MODIFIED, L"a.txt", {}}, {FILE_ACTION_MODIFIED, L"A.txt", {}},
            {FILE_ACTION_MODIFIED, L"a.txt", {}}, {FILE_ACTION_MODIFIED, L"A.txt", {}}};
        check(app::ApplyResolvedDirNotifies(live, app::ResolveDirNotifies(case_path.wstring(), burst)) ==
              app::NotifyPatch::Applied && live.size() == 2 && size_of(live, L"A.txt") == 1 &&
              size_of(live, L"a.txt") == 5,
              "real large modification batch retains independent case identities");
        auto removed = live;
        check(app::ApplyResolvedDirNotifies(removed, app::ResolveDirNotifies(case_path.wstring(), {{FILE_ACTION_REMOVED, L"a.txt", {}}})) == app::NotifyPatch::Applied &&
              NamesAre(removed, {L"A.txt"}), "real remove patch targets the exact case sibling");
        check(app::ApplyResolvedDirNotifies(removed, app::ResolveDirNotifies(case_path.wstring(), {{FILE_ACTION_ADDED, L"a.txt", {}}})) == app::NotifyPatch::Applied &&
              removed.size() == 2 && size_of(removed, L"A.txt") == 1 && size_of(removed, L"a.txt") == 5,
              "real addition leaves the other case sibling intact");
    }
    check(MoveFileW((case_path / L"A.txt").c_str(), (case_path / L"B.txt").c_str()) != FALSE,
          "perform real rename of uppercase sibling");
    {
        auto live = disk;
        check(app::ApplyResolvedDirNotifies(live, app::ResolveDirNotifies(case_path.wstring(), {{FILE_ACTION_RENAMED_NEW_NAME, L"B.txt", L"A.txt"}})) == app::NotifyPatch::Applied &&
              live.size() == 2 && size_of(live, L"B.txt") == 1 && size_of(live, L"a.txt") == 5,
              "real rename patch preserves the unaffected case sibling");
    }
    check(write(L"a.txt", "lower-changed", TRUNCATE_EXISTING), "perform real lowercase modification");
    auto live = std::vector<fs::DirEntry>{Entry(L"B.txt", 1), Entry(L"a.txt", 5)};
    check(app::ApplyResolvedDirNotifies(live, app::ResolveDirNotifies(case_path.wstring(), {{FILE_ACTION_MODIFIED, L"a.txt", {}}})) == app::NotifyPatch::Applied &&
          size_of(live, L"B.txt") == 1 && size_of(live, L"a.txt") == 13,
          "real modified metadata binds to the exact lowercase row");
    std::printf("Case-sensitive name failures: %d\n", failures);
    return failures ? 1 : 0;
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
    if (argc == 2 && wcscmp(argv[1], L"--case-sensitive-names-only") == 0)
        return TestCaseSensitiveNames(fixture);
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
        name_tab.SetSnapshot(name_tab.PrepareEntrySnapshot(app::ProjectDirectorySnapshot(shared, parent,
            name_tab.sort_column, name_tab.sort_direction, app::FolderSortMode::FoldersFirst), names, focus));
        size_tab.SetSnapshot(size_tab.PrepareEntrySnapshot(app::ProjectDirectorySnapshot(shared, parent,
            size_tab.sort_column, size_tab.sort_direction, app::FolderSortMode::FoldersFirst), names, focus));
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
        check(NamesAre(*name_tab.snapshot,
              {L"m-folder", L"0-new.txt", L"a.txt", L"z.txt"}) &&
              name_tab.EntryAt(name_tab.selected_index).name == L"a.txt",
              "background projection sorts newcomers and retains selection across foreign ordering");
        name_tab.PrepareEntryRefresh(true);
        ApplySnapshot(name_tab, *name_tab.snapshot);
        check(NamesAre(*name_tab.snapshot,
              {L"m-folder", L"0-new.txt", L"a.txt", L"z.txt"}) &&
              name_tab.EntryAt(name_tab.selected_index).name == L"a.txt",
              "explicit projection restores sorting while retaining selection");
        for (const auto folders : {app::FolderSortMode::FoldersFirst, app::FolderSortMode::Mixed}) {
            app::Tab tab;
            tab.current_path = parent;
            tab.SetSnapshot(tab.PrepareEntrySnapshot(app::ProjectDirectorySnapshot(shared, parent,
                tab.sort_column, tab.sort_direction, folders), names, focus));
            check(tab.EntryAt(0).name == (folders == app::FolderSortMode::Mixed ? L"a.txt" : L"m-folder"),
                  "folder mode is part of each pane's projection identity");
        }
        const std::vector<fs::DirEntry> shown{Entry(L"unavailable.txt", 42), Entry(L"keep.txt", 7)};
        auto rows = shown;
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(parent, {{FILE_ACTION_MODIFIED, L"unavailable.txt", {}}})) == app::NotifyPatch::NeedFullEnum &&
              NamesAre(rows, {L"unavailable.txt", L"keep.txt"}) && rows[0].size == 42,
              "failed stat preserves old row and requests full enumeration");
        rows = shown;
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(parent, {{FILE_ACTION_ADDED, L"new-unavailable.txt", {}}})) == app::NotifyPatch::NeedFullEnum &&
              NamesAre(rows, {L"unavailable.txt", L"keep.txt"}),
              "unavailable added entry defers without changing existing rows");
        std::vector<fs::DirNotifyEvent> burst(5, {FILE_ACTION_MODIFIED, L"unavailable.txt", {}});
        rows = shown;
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(parent, burst)) == app::NotifyPatch::NeedFullEnum &&
              NamesAre(rows, {L"unavailable.txt", L"keep.txt"}),
              "failed batch stat leaves the original listing intact");
        check(app::ApplyResolvedDirNotifies(rows, app::ResolveDirNotifies(parent, {{FILE_ACTION_REMOVED, L"unavailable.txt", {}}})) == app::NotifyPatch::Applied &&
              NamesAre(rows, {L"keep.txt"}), "explicit removal still deletes the row");

        return failures ? 1 : 0;
    }
    const std::vector<fs::DirEntry> shown{ Entry(L"b.txt", 1), Entry(L"m.txt", 2),
                                           Entry(L"z.txt", 3) };
    const std::vector<app::EntryRenameHint> renames{{L"m.txt", L"temporary.txt"},
                                                  {L"temporary.txt", L"a.txt"}};
    auto fresh = Sorted({Entry(L"a.txt", 20), Entry(L"b.txt", 10), Entry(L"z.txt", 30),
                         Entry(L"0-new.txt", 4), Entry(L"new-folder", 0, true)},
                        ui::SortColumn::Name, ui::SortDirection::Asc, parent);
    const auto newcomers = std::make_shared<std::vector<fs::DirEntry>>(
        std::vector<fs::DirEntry>{Entry(L"z-folder", 0, true), Entry(L"a.txt")});
    check(NamesAre(*app::ProjectDirectorySnapshot(newcomers, parent, ui::SortColumn::Name,
              ui::SortDirection::Asc, app::FolderSortMode::Mixed), {L"a.txt", L"z-folder"}),
          "background projection uses the captured mixed-folder preference");
    check(NamesAre(*app::ProjectDirectorySnapshot(newcomers, parent, ui::SortColumn::Name,
              ui::SortDirection::Desc, app::FolderSortMode::FollowDirection), {L"a.txt", L"z-folder"}),
          "background projection uses the captured direction-following folder preference");
    check(NamesAre(*app::ProjectDirectorySnapshot(newcomers, parent, ui::SortColumn::Name,
              ui::SortDirection::Desc, app::FolderSortMode::FoldersFirst), {L"z-folder", L"a.txt"}),
          "folder preference is an explicit immutable sort input rather than global state");
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
    app::Tab virtual_tab;
    virtual_tab.current_path = app::MakeStarredPath();
    const auto virtual_entries = std::make_shared<std::vector<fs::DirEntry>>(fresh);
    check(virtual_tab.PrepareEntrySnapshot(virtual_entries, names, focus) == virtual_entries &&
          NamesAre(*virtual_entries, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}),
          "virtual results retain the supplied projection");

    app::Tab tab;
    tab.current_path = parent;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(shown));
    tab.SelectOnly(1);
    tab.HoldEntryRename(L"m.txt", L"a.txt");
    tab.PrepareEntryRefresh(false);
    ApplySnapshot(tab, Sorted({Entry(L"a.txt"), Entry(L"b.txt"), Entry(L"z.txt")},
        ui::SortColumn::Name, ui::SortDirection::Asc, parent));
    check(NamesAre(*tab.snapshot, {L"a.txt", L"b.txt", L"z.txt"}) &&
          tab.selected_index == 0 && tab.EntryAt(0).name == L"a.txt" && tab.held_renames.empty(),
          "a refresh arriving before directory notification sorts and follows the renamed selection");
    tab.PrepareEntryRefresh(false);
    ApplySnapshot(tab, fresh);
    check(NamesAre(*tab.snapshot, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}) &&
          tab.EntryAt(tab.selected_index).name == L"a.txt",
          "background full enumeration sorts new files and folders and preserves selection");
    tab.PrepareEntryRefresh(true);
    check(tab.pending_ensure_selection_visible,
          "explicit refresh requests sorted rows and reveals the retained physical selection");
    ApplySnapshot(tab, fresh);
    check(tab.EntryAt(tab.selected_index).name == L"a.txt" &&
          NamesAre(*tab.snapshot, {L"new-folder", L"0-new.txt", L"a.txt", L"b.txt", L"z.txt"}),
          "F5 restores sort order without losing the renamed selection");
    tab.sort_column = ui::SortColumn::Size;
    tab.sort_direction = ui::SortDirection::Desc;
    tab.PrepareEntryRefresh(true);
    ApplySnapshot(tab, Sorted(fresh, tab.sort_column, tab.sort_direction, parent));
    check(tab.EntryAt(tab.selected_index).name == L"a.txt",
          "changing sort also restores sorted order while preserving selection");
    tab.HoldEntryRename(L"a.txt", L"again.txt");
    tab.PrepareEntryRefresh(false);
    check(!tab.NavigateTo(parent) && tab.held_renames.size() == 1 &&
          tab.held_renames[0].new_name == L"again.txt",
          "same-path navigation remains a no-op and preserves a pending rename hint");
    check(tab.NavigateTo(parent + L"\\other") && tab.held_renames.empty(),
          "navigation to a different directory clears pending rename hints");
    tab.current_path = app::MakeHomePath();
    tab.HoldEntryRename(L"a.txt", L"virtual.txt");
    tab.PrepareEntryRefresh(false);
    check(tab.held_renames.empty(), "virtual tabs cannot hold physical rename hints");

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
    auto patched = before;
    check(app::ApplyResolvedDirNotifies(patched, app::ResolveDirNotifies(parent, events)) ==
          app::NotifyPatch::Applied && NamesAre(patched, {L"a.txt", L"folder", L"x.txt", L"c.txt"}) &&
          patched[0].size == 12 && patched[2].size == 7 && patched[3].size == 7,
          "resolved batch handles rename, metadata, deletion and recreation without duplicate rows");
    for (auto& entry : patched)
        entry.mtime = FILETIME{entry.name == L"a.txt" ? 300u : entry.name == L"x.txt" ? 200u :
                              entry.name == L"c.txt" ? 100u : 0u, 0};
    const auto facts = std::make_shared<std::vector<fs::DirEntry>>(patched);
    struct ProjectionCase {
        ui::SortColumn column;
        ui::SortDirection direction;
        std::array<const wchar_t*, 4> names;
    };
    const ProjectionCase projections[] = {
        {ui::SortColumn::Name, ui::SortDirection::Asc, {L"folder", L"a.txt", L"c.txt", L"x.txt"}},
        {ui::SortColumn::Name, ui::SortDirection::Desc, {L"folder", L"x.txt", L"c.txt", L"a.txt"}},
        {ui::SortColumn::Size, ui::SortDirection::Asc, {L"folder", L"c.txt", L"x.txt", L"a.txt"}},
        {ui::SortColumn::Size, ui::SortDirection::Desc, {L"folder", L"a.txt", L"x.txt", L"c.txt"}},
        {ui::SortColumn::Mtime, ui::SortDirection::Asc, {L"folder", L"c.txt", L"x.txt", L"a.txt"}},
        {ui::SortColumn::Mtime, ui::SortDirection::Desc, {L"a.txt", L"x.txt", L"c.txt", L"folder"}},
        {ui::SortColumn::Type, ui::SortDirection::Asc, {L"folder", L"a.txt", L"c.txt", L"x.txt"}},
        {ui::SortColumn::Type, ui::SortDirection::Desc, {L"folder", L"x.txt", L"c.txt", L"a.txt"}}
    };
    for (const auto& expected : projections) {
        const auto projected = app::ProjectDirectorySnapshot(facts, parent, expected.column,
            expected.direction, app::FolderSortMode::FoldersFirst);
        check(projected && projected->size() == expected.names.size() &&
              std::equal(projected->begin(), projected->end(), expected.names.begin(),
                  [](const auto& entry, const auto* name) { return entry.name == name; }) &&
              NamesAre(*facts, {L"a.txt", L"folder", L"x.txt", L"c.txt"}),
              "resolved facts project to the specified order without changing the shared source");
    }
    auto duplicates = std::vector<fs::DirEntry>{Entry(L"d.txt"), Entry(L"b.txt"),
                                               Entry(L"a.txt"), Entry(L"c.txt")};
    check(app::ApplyResolvedDirNotifies(duplicates, app::ResolveDirNotifies(parent, {events[0]})) == app::NotifyPatch::Applied &&
          NamesAre(duplicates, {L"d.txt", L"a.txt", L"c.txt"}),
          "rename collision removes the duplicate while retaining the renamed row's slot");
    const std::vector<fs::DirNotifyEvent> vanished_intermediate{
        {FILE_ACTION_RENAMED_NEW_NAME, L"temporary.txt", L"b.txt"},
        {FILE_ACTION_RENAMED_NEW_NAME, L"a.txt", L"temporary.txt"}};
    auto rename_batch = before;
    auto rename_single = before;
    check(app::ApplyResolvedDirNotifies(rename_batch, app::ResolveDirNotifies(parent, vanished_intermediate)) == app::NotifyPatch::NeedFullEnum &&
          app::ApplyResolvedDirNotifies(rename_single, app::ResolveDirNotifies(parent, {vanished_intermediate.front()})) == app::NotifyPatch::NeedFullEnum &&
          NamesAre(rename_batch, {L"d.txt", L"b.txt", L"c.txt", L"folder"}) &&
          NamesAre(rename_single, {L"d.txt", L"b.txt", L"c.txt", L"folder"}),
          "vanished rename intermediates defer atomically without deleting the held source slot");
    const std::vector<app::EntryRenameHint> queued_renames{{L"b.txt", L"temporary.txt"},
                                                          {L"temporary.txt", L"a.txt"}};
    const auto after_rename_enum = app::ProjectDirectorySnapshot(
        std::make_shared<std::vector<fs::DirEntry>>(std::vector<fs::DirEntry>{
            Entry(L"a.txt"), Entry(L"c.txt"), Entry(L"d.txt"), Entry(L"folder", 0, true)}),
        parent, ui::SortColumn::Name, ui::SortDirection::Asc, app::FolderSortMode::FoldersFirst);
    names = {L"b.txt"};
    focus = L"b.txt";
    app::FollowEntryRenames(queued_renames, *after_rename_enum, names, focus);
    check(NamesAre(*after_rename_enum, {L"folder", L"a.txt", L"c.txt", L"d.txt"}) &&
          names[0] == L"a.txt" && focus == L"a.txt",
          "fallback enumeration restores sorted rows and follows the final rename selection");
    auto missing = before;
    check(app::ApplyResolvedDirNotifies(missing, app::ResolveDirNotifies(parent, {{FILE_ACTION_MODIFIED, L"b.txt", {}}})) == app::NotifyPatch::NeedFullEnum &&
          NamesAre(missing, {L"d.txt", L"b.txt", L"c.txt", L"folder"}),
          "a failed touched-file stat retains its held row pending full enumeration");
    auto invalid = before;
    check(app::ApplyResolvedDirNotifies(invalid, app::ResolveDirNotifies(parent, {{FILE_ACTION_ADDED, L"child\\invalid.txt", {}}})) == app::NotifyPatch::NeedFullEnum &&
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
    const auto localized = app::ProjectDirectorySnapshot(
        std::make_shared<std::vector<fs::DirEntry>>(std::vector<fs::DirEntry>{pictures, downloads}),
        parent, ui::SortColumn::Name, ui::SortDirection::Asc, app::FolderSortMode::FoldersFirst);
    check(localized->size() == 2 && localized->front().name ==
          (localized_first ? downloads.name : pictures.name),
          "background projection uses localized EntryLess with the physical parent");
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
    std::printf("Directory model failures: %d\n", failures);
    return failures ? 1 : 0;
}
