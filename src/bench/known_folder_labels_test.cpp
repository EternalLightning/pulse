#include "../common/known_folder_labels.h"
#include "../app/entry_sort.h"
#include <shlwapi.h>
#include <iostream>

int wmain() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    path::InitializeKnownFolderLabels();
    bool ok = true;
    auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        ok &= passed;
    };
    const std::array<path::KnownFolderLabel, 3> redirected{{
        {L"D:\\Photos", l10n::StringId::KnownFolderPictures},
        {L"E:\\My Downloads", l10n::StringId::Downloads},
        {L"\\\\server\\share\\Clips", l10n::StringId::KnownFolderVideos}}};
    const auto* photos = path::FindKnownFolderLabel(L"\\\\?\\d:\\PHOTOS\\", redirected);
    check(photos && photos->label == l10n::StringId::KnownFolderPictures,
        "redirected pictures match actual path despite physical basename");
    check(path::FindKnownFolderLabel(L"e:/My Downloads/", redirected) == &redirected[1],
        "redirected downloads accept slash and trailing separator");
    check(path::FindKnownFolderLabel(L"\\\\?\\UNC\\server\\share\\Clips", redirected) == &redirected[2],
        "redirected UNC videos accept extended paths");
    check(!path::FindKnownFolderLabel(L"C:\\Photos", redirected) &&
          !path::FindKnownFolderLabel(L"D:\\Photos\\Vacation", redirected) &&
          !path::FindKnownFolderLabel(L"D:\\Photos2", redirected),
        "ordinary same-named folders and children retain physical names");
    check(l10n::Get(l10n::StringId::KnownFolderPictures) == L"图片" &&
          l10n::Get(l10n::StringId::KnownFolderVideos) == L"视频",
        "Chinese pictures and videos labels exist");
    for (const auto& folder : path::KnownFolderLabels()) {
        if (!folder.path.empty()) {
            check(path::KnownFolderDisplayName(folder.path, L"physical name") == l10n::Get(folder.label),
                "actual known folder location uses localized label");
        }
    }
    auto sort_checks = [&] {
        const auto saved = path::KnownFolderLabels();
        // Isolated labels exercise redirected paths without creating folders or
        // depending on the user's actual Known Folder locations.
        auto& folders = path::KnownFolderLabels();
        folders[1].path = L"D:\\PulseSort\\Downloads";
        folders[2].path = L"D:\\PulseSort\\Pictures";
        fs::DirEntry downloads, pictures;
        downloads.name = L"Downloads";
        pictures.name = L"Pictures";
        downloads.is_dir = pictures.is_dir = true;
        const bool expected = StrCmpLogicalW(l10n::Get(folders[1].label).c_str(),
            l10n::Get(folders[2].label).c_str()) < 0;
        const auto physical_downloads = downloads.name;
        const auto physical_pictures = pictures.name;
        for (const auto column : {ui::SortColumn::Name, ui::SortColumn::Size,
                ui::SortColumn::Mtime, ui::SortColumn::Type, ui::SortColumn::Path}) {
            check(app::EntryLess(downloads, pictures, column, ui::SortDirection::Asc,
                      L"D:\\PulseSort") == expected &&
                  app::EntryLess(downloads, pictures, column, ui::SortDirection::Desc,
                      L"D:\\PulseSort") != expected,
                "directory names and equal-key tie breaks follow localized order in both directions");
        }
        check(app::EntryLess(downloads, pictures, ui::SortColumn::Name, ui::SortDirection::Asc,
                  L"D:\\Ordinary") && downloads.name == physical_downloads && pictures.name == physical_pictures,
            "same-named ordinary folders retain physical order and operation names");
        downloads.name = expected ? L"Z redirected" : L"A redirected";
        pictures.name = expected ? L"A redirected" : L"Z redirected";
        folders[1].path = L"D:\\PulseSort\\" + downloads.name;
        folders[2].path = L"D:\\PulseSort\\" + pictures.name;
        check(app::EntryLess(downloads, pictures, ui::SortColumn::Name, ui::SortDirection::Asc,
                  L"D:\\PulseSort") == expected,
            "redirected physical names cannot override localized order");
        downloads.name = physical_downloads;
        pictures.name = physical_pictures;
        folders[1].path = L"D:\\PulseSort\\Downloads";
        folders[2].path = L"D:\\PulseSort\\Pictures";
        downloads.full_path = L"\\\\?\\D:\\PulseSort\\Downloads";
        pictures.full_path = L"D:\\PulseSort\\Pictures";
        check(app::EntryLess(downloads, pictures, ui::SortColumn::Name, ui::SortDirection::Asc,
                  L"pulse:tag:fixture") == expected,
            "virtual views and extended paths use the actual folder location for sorting");
        downloads.change_record_only = pictures.change_record_only = true;
        check(app::EntryLess(downloads, pictures, ui::SortColumn::Name, ui::SortDirection::Asc),
            "historical records retain their original names");
        path::KnownFolderLabels() = saved;
    };
    sort_checks();
    l10n::SetLanguage(L"en-US");
    sort_checks();
    check(l10n::Get(l10n::StringId::KnownFolderPictures) == L"Pictures" &&
          l10n::Get(l10n::StringId::KnownFolderVideos) == L"Videos",
        "English labels follow language switching");
    check(path::KnownFolderDisplayName(L"Z:\\PulseOrdinaryFolder", L"PulseOrdinaryFolder") == L"PulseOrdinaryFolder",
        "unknown location preserves fallback");
    return ok ? 0 : 1;
}
