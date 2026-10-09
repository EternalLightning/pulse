#include "../app/app_model.h"
#include "../app/context_menu.h"
#include "../app/drive_label_rename.h"
#include "../app/entry_sort.h"
#include "../common/localization.h"
#include <algorithm>
#include <cstdio>

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
    check(contains(home, app::CmdOpen) && contains(home, app::CmdOpenInNewTab) &&
          contains(home, app::CmdCopyPath) && contains(home, app::CmdProperties),
          "home drive cards have path commands");
    check(contains(app::BuildRecyclePlaceMenu(true), app::CmdEmptyRecycle),
          "home recycle card has recycle commands");
    return failures ? 1 : 0;
}
