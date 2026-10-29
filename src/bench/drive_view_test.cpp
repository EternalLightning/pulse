#include "../app/app_model.h"
#include "../app/context_menu.h"
#include "../app/drive_label_rename.h"
#include "../app/entry_sort.h"
#include "../common/localization.h"
#include <algorithm>
#include <cstdio>
#include <chrono>
#include "../common/path_utils.h"

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int wmain() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::vector<fs::DirEntry> drives(3);
    drives[0].name = L"Applications (D:)"; drives[0].full_path = L"\\\\?\\D:\\";
    drives[1].name = L"Windows 11 (C:)"; drives[1].full_path = L"C:\\";
    drives[2].name = L"Backup (F:)"; drives[2].full_path = L"F:\\";
    for (auto& drive : drives) drive.is_dir = true;
    const auto sort = [&](ui::SortDirection direction, std::wstring_view parent) {
        std::sort(drives.begin(), drives.end(), [&](const auto& a, const auto& b) {
            return app::EntryLess(a, b, ui::SortColumn::Name, direction, parent);
        });
    };
    sort(ui::SortDirection::Asc, L"");
    check(drives[0].name == L"Windows 11 (C:)" && drives[1].name == L"Applications (D:)",
          "This PC sorts by drive letter independently of labels");
    sort(ui::SortDirection::Desc, L"");
    check(drives[0].name == L"Backup (F:)" && drives[2].name == L"Windows 11 (C:)",
          "This PC descending reverses drive letters");
    sort(ui::SortDirection::Asc, L"pulse:search:");
    check(drives[0].name == L"Applications (D:)", "other views keep name sorting");
    check(app::DriveLabelFromDisplayName(L"\\\\?\\C:\\", L"Windows 11 (C:)") == L"Windows 11" &&
          app::DriveLabelFromDisplayName(L"C:\\", L"C:").empty(), "rename edits only the volume label");
    check(app::DriveDisplayName(L"C:\\", L"") == L"C:" &&
          app::DriveDisplayName(L"C:\\", L"Data (C:)") == L"Data (C:) (C:)",
          "empty and parenthesized labels preserve the drive letter");
    const auto contains = [](const auto& items, int command) {
        for (const auto& item : items) {
            if (item.command == command) return true;
            for (const auto& swatch : item.quick_swatches)
                if (swatch.command == command) return true;
        }
        return false;
    };
    auto items = app::BuildItemMenu(false, L"", true);
    const auto position = [&](int command) {
        return std::find_if(items.begin(), items.end(), [command](const auto& item) {
            return item.command == command;
        }) - items.begin();
    };
    app::AppendTextCompareCommands(items, true, L"C:\\fixture\\file.txt", true);
    app::AppendRecentChangesCommand(items, L"C:\\fixture", true);
    app::OrganizeItemMenu(items);
    check(!items.front().quick_swatches.empty() && items.front().separator_after &&
          position(app::CmdOpen) < position(app::CmdOpenInNewTab) &&
          position(app::CmdOpenInNewTab) < position(app::CmdOpenTerminal) &&
          position(app::CmdOpenTerminal) < position(app::CmdCopyPath),
          "item menu keeps common buttons first and groups open and navigation ahead of file operations");
    check(position(app::CmdCopyPath) < position(app::CmdSelectTextCompare) &&
          position(app::CmdSelectTextCompare) < position(app::CmdCompareSelectedText) &&
          position(app::CmdCompareSelectedText) < position(app::CmdViewRecentChanges) &&
          position(app::CmdViewRecentChanges) < position(app::CmdPinWorkspace) &&
          items.back().command == app::CmdProperties && !items.back().separator_after,
          "injected comparison and recent changes actions remain grouped before organization and final properties");
    app::RestrictDriveItemMenu(items, true);
    check(!contains(items, app::CmdCut) && !contains(items, app::CmdCopy) &&
          !contains(items, app::CmdDelete) && contains(items, app::CmdRename),
          "single drive menu keeps rename and removes file mutations");
    app::RestrictDriveItemMenu(items, false);
    check(!contains(items, app::CmdRename), "multiple drive selection has no file rename strip");
    check(!contains(app::BuildBreadcrumbMenu(true, true), app::CmdCopy) &&
          contains(app::BuildBreadcrumbMenu(true, false), app::CmdCopy),
          "home and breadcrumb drive menus forbid copy while folders keep it");
    const auto home = app::BuildBreadcrumbMenu(true, true);
    const auto home_background = app::BuildHomeBackgroundMenu();
    check(home_background.size() == 1 && contains(home_background, app::CmdRefresh) &&
          !contains(home_background, app::CmdNewMenu) && !contains(home_background, app::CmdPaste) &&
          !contains(home_background, app::CmdOpenTerminal) && !contains(home_background, app::CmdCopyPath),
          "home background offers refresh without folder creation, paste or path commands");
    check(contains(home, app::CmdOpen) && contains(home, app::CmdOpenInNewTab) &&
          contains(home, app::CmdCopyPath) && contains(home, app::CmdProperties),
          "home drive cards have path commands");
    check(contains(app::BuildRecyclePlaceMenu(true), app::CmdEmptyRecycle),
          "home recycle card has recycle commands");
    app::Tab selection;
    selection.current_path = LR"(C:\fixture)";
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>(drives);
    selection.SetSnapshot(snapshot);
    selection.SelectOnly(0);
    check(selection.SnapshotSelectionHasDriveRoot(), "cached selection aggregate detects drive roots");
    snapshot->front().full_path = LR"(C:\folder)";
    selection.SetSnapshot(snapshot);
    check(!selection.SnapshotSelectionHasDriveRoot(), "publishing a snapshot invalidates cached aggregates even at the same address");
    snapshot->front().full_path = drives.front().full_path;
    selection.SetSnapshot(snapshot);
    selection.ClearSelection();
    check(!selection.SnapshotSelectionHasDriveRoot(), "clearing selection invalidates drive protection");
    auto next = std::make_shared<std::vector<fs::DirEntry>>(*snapshot);
    next->front().change_record_only = true;
    selection.SetSnapshot(next);
    selection.SelectOnly(0);
    check(!selection.SnapshotSelectionHasDriveRoot(), "record-only rows do not enable drive protection");
    next = std::make_shared<std::vector<fs::DirEntry>>(*snapshot);
    next->front().full_path = LR"(C:\folder)";
    selection.SetSnapshot(next);
    selection.SelectOnly(0);
    check(!selection.SnapshotSelectionHasDriveRoot(), "snapshot replacement invalidates the cached root flag");
    selection.SelectAll();
    check(selection.SnapshotSelectionHasDriveRoot(), "select-all aggregate detects roots beyond the focused row");

    selection.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1));
    selection.SelectOnly(0);
    selection.current_path = L"C:";
    check(selection.SnapshotSelectionHasDriveRoot(), "empty-name fallback preserves joined drive-root path");
    selection.current_path = L"C:/";
    check(!selection.SnapshotSelectionHasDriveRoot(), "parent-path change invalidates fallback without snapshot replacement");
    selection.current_path = LR"(C:\fixture)";
    auto bulk = std::make_shared<std::vector<fs::DirEntry>>(20000);
    for (auto& row : *bulk) {
        row.name = std::wstring(120, L'x') + L".txt";
        row.full_path = LR"(C:\fixture\)" + row.name;
    }
    selection.SetSnapshot(bulk);
    selection.SelectAll();
    selection.SnapshotSelectionHasDriveRoot();
    const auto legacy = [&] {
        const auto indices = selection.SelectedIndices();
        std::vector<std::wstring> paths;
        paths.reserve(indices.size());
        for (int index : indices) paths.push_back(selection.EntryAt(index).full_path);
        return std::any_of(paths.begin(), paths.end(), path::IsDriveRootPath);
    };
    check(!legacy() && !selection.SnapshotSelectionHasDriveRoot(),
          "20k selected files preserve the legacy root predicate");
    constexpr int rounds = 30;
    volatile int result = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) result = result + legacy();
    const auto middle = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) result = result + selection.SnapshotSelectionHasDriveRoot();
    const auto end = std::chrono::steady_clock::now();
    std::printf("[BENCH] 20k selected rows, %d warm checks: legacy %.3f ms, cached %.3f ms (result=%d)\n",
        rounds, std::chrono::duration<double, std::milli>(middle - start).count(),
        std::chrono::duration<double, std::milli>(end - middle).count(), static_cast<int>(result));
    return failures ? 1 : 0;
}
