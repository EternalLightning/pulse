// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Link production objects with scripts/test_integration_fixture.ps1.
#include "../app/app_internal.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include <cstdio>
#include <filesystem>

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    wchar_t temporary[32768]{};
    if (!GetTempPathW(ARRAYSIZE(temporary), temporary)) return 2;
    const auto root = std::filesystem::path(temporary) / (L"pulse-recycle-preferences-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting(root.wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Recycle preferences fixture",
        WS_OVERLAPPEDWINDOW, -30000, -30000, 1100, 720, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    const auto recycle = app::MakeRecyclePath();
    auto* tab = ActiveTab(s);
    tab->view_mode = ui::ViewMode::Tiles;
    tab->sort_column = ui::SortColumn::Size;
    tab->sort_direction = ui::SortDirection::Desc;
    StartLoadingPath(s, *tab, recycle);
    check(tab->view_mode == ui::ViewMode::Details && tab->sort_column == ui::SortColumn::Name &&
        tab->sort_direction == ui::SortDirection::Asc,
        "fresh recycle view uses Details and Name ascending regardless of the previous folder");

    SetViewMode(s, ui::ViewMode::Tiles);
    SetSort(s, ui::SortColumn::Size, ui::SortDirection::Desc);
    check(tab->view_mode == ui::ViewMode::Tiles && tab->sort_column == ui::SortColumn::Size &&
        tab->sort_direction == ui::SortDirection::Desc,
        "production view and sort commands update the recycle tab");
    std::wstring json;
    check(ReadUtf8File(app::GetPulseDataDir() + L"\\app.json", json),
        "production commands save preferences to isolated app.json");
    app::AppPrefs restored;
    restored.persist = false;
    check(restored.FromJson(json) && restored.folder_views.Find(recycle) == ui::ViewMode::Tiles &&
        restored.folder_views.FindSort(recycle) == app::FolderSortPrefs{ui::SortColumn::Size, ui::SortDirection::Desc},
        "saved recycle view and sort survive the production JSON roundtrip");
    s.appPrefs.folder_views = restored.folder_views;
    s.pane->NewTab(L"C:\\recycle-preferences-fixture");
    tab = ActiveTab(s);
    tab->view_mode = ui::ViewMode::Details;
    tab->sort_column = ui::SortColumn::Mtime;
    tab->sort_direction = ui::SortDirection::Asc;
    StartLoadingPath(s, *tab, recycle);
    check(tab->view_mode == ui::ViewMode::Tiles && tab->sort_column == ui::SortColumn::Size &&
        tab->sort_direction == ui::SortDirection::Desc,
        "opening recycle in a different tab restores its saved view and sort");

    for (const auto* path : {L"pulse:home", L"pulse:settings", L"pulse:search:test", L"pulse:recycle:other"}) {
        check(!s.appPrefs.folder_views.Set(path, ui::ViewMode::Tiles) &&
            !s.appPrefs.folder_views.SetSort(path, ui::SortColumn::Size, ui::SortDirection::Desc) &&
            !s.appPrefs.folder_views.Find(path) && !s.appPrefs.folder_views.FindSort(path),
            "unrelated virtual paths still reject view and sort preferences");
    }
    check(!s.appPrefs.folder_views.SetGroup(recycle, ui::GroupMode::Size, ui::SortDirection::Desc) &&
        !s.appPrefs.folder_views.SetColumns(recycle, {100.0f, 200.0f, 300.0f, 400.0f}) &&
        !s.appPrefs.folder_views.FindGroup(recycle) && !s.appPrefs.folder_views.FindColumns(recycle),
        "recycle grouping and column preferences remain unsupported");
    s.shot.active = true;
    tab->view_mode = ui::ViewMode::Tiles;
    tab->sort_column = ui::SortColumn::Mtime;
    tab->sort_direction = ui::SortDirection::Asc;
    StartLoadingPath(s, *tab, recycle);
    check(tab->view_mode == ui::ViewMode::Details && tab->sort_column == ui::SortColumn::Mtime &&
        tab->sort_direction == ui::SortDirection::Asc,
        "shot mode keeps its existing recycle defaults and ignores saved preferences");
    s.shot.active = false;
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
