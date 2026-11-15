// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/session.h"
#include "../app/tab_group_id.h"
#include "../app/places.h"
#include "../app/app_prefs.h"
#include "../app/context_menu_prefs.h"
#include "../common/json_utils.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include <filesystem>
#include <iostream>
#include <string_view>
#include "places_tag_loading_cases.h"

static int RunPreferencesLoadProtectionChecks() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !passed;
    };
    const auto parent = std::filesystem::absolute(L"bench_data");
    const auto root = parent / (L"preferences-load-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return 2;
    storage::OverrideDefaultRootForTesting(root.wstring());
    const auto check_file = [&](auto& prefs, const wchar_t* filename, const std::wstring& valid) {
        const auto path = (root / filename).wstring();
        const auto load = [&] {
            // AppPrefs must not repair the user's startup registry entry in a fixture.
            prefs.persist = false;
            const bool loaded = prefs.Load();
            prefs.persist = true;
            return loaded;
        };
        check(!load() && prefs.Save(), "missing preferences permit first-run creation");
        for (const auto& damaged : {std::wstring(), std::wstring(L"{damaged"), std::wstring(L"[]")}) {
            check(WriteUtf8FileAtomic(path, damaged) && !load(),
                "empty, damaged and non-object preferences report load failure");
            std::wstring retained;
            check(!prefs.Save() && ReadUtf8File(path, retained) && retained == damaged,
                "failed preference load prevents automatic replacement");
        }
        check(WriteUtf8FileAtomic(path, valid), "write valid preferences before exclusive lock");
        HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "lock existing preferences against reads");
        if (lock != INVALID_HANDLE_VALUE) {
            check(!load(), "locked preference file reports load failure");
            CloseHandle(lock);
            std::wstring retained;
            check(!prefs.Save() && ReadUtf8File(path, retained) && retained == valid,
                "unlocking after failed load still preserves original preferences on save");
        }
        check(!prefs.FromJson(L"{damaged") && !prefs.Save(),
            "failed import does not clear load protection");
        check(load() && prefs.Save(), "successful reload restores preference saves");
        check(!prefs.FromJson(L"{damaged") && prefs.Save(),
            "failed import does not disable an otherwise healthy preference store");
        check(WriteUtf8FileAtomic(path, L"{damaged") && !load(),
            "prepare failed load before explicit reset");
        prefs.ResetToDefaults();
        check(prefs.Save(), "explicit reset permits replacing damaged preferences");
        check(WriteUtf8FileAtomic(path, L"{damaged") && !load() &&
            prefs.FromJson(valid) && prefs.Save(), "valid import permits replacing damaged preferences");
    };
    app::AppPrefs prefs;
    check_file(prefs, L"app.json", LR"({"row_height":42})");
    app::ContextMenuPrefs menus;
    check_file(menus, L"context_menu.json", LR"({"categories":{"software":false}})");

    const auto path = (root / L"preferences.json").wstring();
    const auto legacy = (root / L"session.json").wstring();
    app::WindowSessionSnapshot loaded;
    bool saves_enabled = false;
    check(!app::LoadSessionPreferences(loaded, &saves_enabled) && saves_enabled &&
        app::SaveSessionPreferences(loaded), "missing window preferences permit first-run creation");
    const std::wstring valid = LR"({"sidebarHidden":3})";
    check(WriteUtf8FileAtomic(legacy, LR"({"sidebarHidden":1})") &&
        WriteUtf8FileAtomic(path, valid), "write independent current and legacy window preferences");
    for (const auto& damaged : {std::wstring(), std::wstring(L"{damaged"), std::wstring(L"[]")}) {
        check(WriteUtf8FileAtomic(path, damaged) &&
            !app::LoadSessionPreferences(loaded, &saves_enabled) && !saves_enabled,
            "invalid window preferences disable automatic saves without falling back to legacy");
        // The application recaptures a fresh snapshot when closing any window.
        const app::WindowSessionSnapshot captured;
        if (saves_enabled) app::SaveSessionPreferences(captured);
        std::wstring retained;
        check(ReadUtf8File(path, retained) && retained == damaged,
            "recaptured window defaults retain the shared load-failure protection");
    }
    check(WriteUtf8FileAtomic(path, valid), "restore window preferences before exclusive lock");
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "lock existing window preferences against reads");
    if (lock != INVALID_HANDLE_VALUE) {
        check(!app::LoadSessionPreferences(loaded, &saves_enabled) && !saves_enabled,
            "locked window preferences disable automatic saves");
        CloseHandle(lock);
        if (saves_enabled) app::SaveSessionPreferences({});
        std::wstring retained;
        check(ReadUtf8File(path, retained) && retained == valid,
            "unlocking window preferences before exit preserves original values");
    }
    check(app::LoadSessionPreferences(loaded, &saves_enabled) && saves_enabled &&
        loaded.sidebar_hidden == 3, "successful window preference reload restores automatic saves");
    std::filesystem::remove(path, error);
    check(!error && app::LoadSessionPreferences(loaded, &saves_enabled) && saves_enabled &&
        loaded.sidebar_hidden == 1 && app::SaveSessionPreferences(loaded),
        "readable legacy window preferences can still migrate into their new file");
    std::filesystem::remove(path, error);
    check(!error && WriteUtf8FileAtomic(legacy, L"{damaged") &&
        !app::LoadSessionPreferences(loaded, &saves_enabled) && !saves_enabled,
        "unreadable legacy window preferences do not authorize saving fallback defaults");
    storage::OverrideDefaultRootForTesting(L"");
    const auto actual = std::filesystem::weakly_canonical(root, error);
    const auto expected_parent = std::filesystem::weakly_canonical(parent, error);
    if (!error && actual.parent_path() == expected_parent &&
        actual.filename().wstring().starts_with(L"preferences-load-"))
        std::filesystem::remove_all(actual, error);
    else check(false, "load protection fixture cleanup remains inside bench_data");
    check(!error, "load protection fixture cleaned without user preferences");
    return failures ? 1 : 0;
}

static int RunSessionPreferencesChecks() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << name << '\n';
        failures += !passed;
    };
    const auto root = std::filesystem::absolute(L"bench_data") /
        (L"session-preferences-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code fixture_error;
    std::filesystem::create_directories(root, fixture_error);
    if (fixture_error) {
        std::cerr << "[FAIL] create session preference fixture: error=" << fixture_error.value() << '\n';
        return 2;
    }
    storage::OverrideDefaultRootForTesting(root.wstring());
    const std::wstring legacy_undo = LR"("undo":[{"type":2,"sup":true,"dest":"","name":"renamed.txt","src":["C:\\fixture\\original.txt"]}],)";
    const std::wstring legacy_tray = LR"("tray":[{"move":true,"items":["C:\\fixture\\staged.txt"]}],)";
    const std::wstring legacy_json = LR"({"version":8,"sidebarHidden":1,"path":"C:\\retained","layoutTabs":[{"panes":[{"path":"C:\\retained"}]}],"undo":[{"type":2,"src":["C:\\fixture\\original.txt"]}],"tray":[{"move":true,"items":["C:\\fixture\\staged.txt"]}],"windows":[{"path":"C:\\retained","tray":[]}]})";
    check(WriteUtf8FileAtomic((root / L"session.json").wstring(), legacy_json),
        "write an isolated legacy session with a completed rename in undo history");
    app::WindowSessionSnapshot preferences;
    check(app::LoadSessionPreferences(preferences) && preferences.sidebar_hidden == 1,
        "preferences migrate legacy settings without restoring navigation");
    const auto migrated_json = app::SessionPreferencesToJson(preferences);
    const json::Document migrated(migrated_json);
    check(migrated.Is(json::Kind::Object) && !migrated.Find(L"undo"),
        "legacy session undo history is discarded during preference migration");
    check(!migrated.Find(L"tray"), "legacy session staging tray is discarded during preference migration");
    std::wstring original_session;
    check(ReadUtf8File((root / L"session.json").wstring(), original_session),
        "capture the complete legacy session before saving preferences");
    preferences.sidebar_hidden = 2;
    check(app::SaveSessionPreferences(preferences), "save preferences into their separate file");
    std::wstring saved_session, saved_preferences;
    check(ReadUtf8File((root / L"session.json").wstring(), saved_session) && saved_session == original_session &&
        ReadUtf8File((root / L"preferences.json").wstring(), saved_preferences),
        "saving preferences preserves the complete session bytes");
    const json::Document doc(saved_preferences);
    check(doc.Is(json::Kind::Object) && !doc.Find(L"path") && !doc.Find(L"layoutTabs") && !doc.Find(L"windows"),
        "the preference schema contains no navigation state");
    check(!doc.Find(L"undo"), "saved preferences contain no undo history");
    check(!doc.Find(L"tray"), "saved preferences contain no staging tray");
    check(app::LoadSessionPreferences(preferences) && preferences.sidebar_hidden == 2,
        "the new preference file takes precedence over legacy values");
    auto legacy_preferences = saved_preferences;
    legacy_preferences.insert(legacy_preferences.find(L'{') + 1, legacy_undo + legacy_tray);
    check(WriteUtf8FileAtomic((root / L"preferences.json").wstring(), legacy_preferences) &&
        app::LoadSessionPreferences(preferences) && preferences.sidebar_hidden == 2 &&
        app::SaveSessionPreferences(preferences) &&
        ReadUtf8File((root / L"preferences.json").wstring(), saved_session) && saved_session == saved_preferences,
        "reopening old preferences forgets undo and staging tray while preserving other settings");
    preferences.sidebar_order.assign(json::kMaxDocumentChars / 2, 0);
    check(!app::SaveSessionPreferences(preferences) &&
        ReadUtf8File((root / L"preferences.json").wstring(), saved_session) && saved_session == saved_preferences,
        "oversized preferences fail before replacing the previous file");
    check(WriteUtf8FileAtomic((root / L"preferences.json").wstring(), L"{damaged") &&
        !app::LoadSessionPreferences(preferences),
        "a damaged new preference file does not silently resurrect legacy values");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--preferences-load-protection")
        return RunPreferencesLoadProtectionChecks();
    if (argc == 2 && std::string_view(argv[1]) == "--session-preferences")
        return RunSessionPreferencesChecks();
    if (argc == 2 && std::string_view(argv[1]) == "--places-tags-load")
        return RunPlacesTagLoadingCases();
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok;
    };
    if (argc == 2 && std::string_view(argv[1]) == "--folder-size-preferences") {
        app::AppPrefs prefs;
        check(prefs.auto_folder_size && prefs.FromJson(L"{}") && prefs.auto_folder_size,
            "new and legacy preferences keep automatic folder size enabled");
        prefs.auto_folder_size = false;
        const app::FolderSortPrefs this_pc{ui::SortColumn::Name, ui::SortDirection::Desc};
        const app::FolderSortPrefs folder{ui::SortColumn::Mtime, ui::SortDirection::Asc};
        const auto path = LR"(C:\fixture)";
        check(prefs.folder_views.SetSort(L"", this_pc.column, this_pc.direction) &&
            prefs.folder_views.SetSort(path, folder.column, folder.direction),
            "This PC and an ordinary directory accept independent sort preferences");
        app::AppPrefs loaded;
        check(loaded.FromJson(prefs.ToJson()) && !loaded.auto_folder_size &&
            loaded.folder_views.FindSort(L"") == this_pc && loaded.folder_views.FindSort(path) == folder,
            "disabled automatic size and separate sort preferences survive JSON round trip");
        loaded.ResetToDefaults();
        check(loaded.auto_folder_size && !loaded.folder_views.FindSort(L""),
            "reset restores automatic folder size and the default This PC sort");
        return failures ? 1 : 0;
    }
    {
        app::WindowTabs tabs;
        const int maximum = (std::numeric_limits<int>::max)();
        tabs.tab_groups = {{maximum, L"maximum", 0, false}, {1, L"first", 0, false}};
        tabs.next_tab_group_id = app::NextTabGroupId(maximum);
        check(tabs.next_tab_group_id == 1 && app::AllocateTabGroupId(tabs) == 2,
              "INT_MAX group counter wraps without colliding with an existing group");
        tabs.tab_groups.clear();
        tabs.next_tab_group_id = maximum;
        check(app::AllocateTabGroupId(tabs) == maximum && tabs.next_tab_group_id == 1 &&
              app::AllocateTabGroupId(tabs) == 1,
              "group allocation safely wraps after INT_MAX");
        tabs.next_tab_group_id = -1;
        tabs.tab_groups.push_back({1, L"first", 0, false});
        check(app::AllocateTabGroupId(tabs) == 2,
              "group allocation normalizes invalid counters and skips occupied IDs");
    }
    if (argc == 2 && std::string_view(argv[1]) == "--tab-group-id") return failures ? 1 : 0;
    const auto parent = std::filesystem::current_path() / L"bench_data";
    const auto root = parent / (L"json-persistence-" + std::to_wstring(GetCurrentProcessId()) +
                               L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting(root.wstring());
    {
        const std::wstring old_session = LR"({"version":7,"left":1,"top":2,"right":800,"bottom":600,"path":"C:\\old","tray":[],"undo":[],"layoutTabs":[{"title":"legacy","panes":[{"path":"C:\\pane","view":"details","cols":"0,0,0"}]}]})";
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(), old_session), "write isolated actual legacy session shape");
        app::WindowSessionSnapshot loaded;
        check(app::LoadSessionPreferences(loaded) && loaded.window_rect.left == 1,
              "legacy session preferences remain readable alongside ignored navigation");
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(),
              LR"({"child":{"left":999,"path":"fake"},"left":5,"path":"real","tray":[],"undo":[]})") &&
              app::LoadSessionPreferences(loaded) && loaded.window_rect.left == 5,
              "production preference migration selects root members over nested pseudo-state");
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(), LR"({"left":truejunk})") &&
              !app::LoadSessionPreferences(loaded) && loaded.window_rect.left == 5,
              "damaged legacy preferences rejected before replacing current snapshot");
        const std::wstring tray_json = LR"([{"move":false,"items":["C:\\fixture\\\u6C49\uD83D\uDE00\b\f"]}])";
        app::StagingTray tray;
        std::wstring serialized;
        check(tray.FromJson(tray_json), "session tray reader accepts complete Unicode/control escapes");
        tray.ToJson(serialized);
        const json::Document tray_doc(serialized);
        check(tray_doc.Is(json::Kind::Array) && !tray_doc.Root().children.empty() &&
              json::Strings(tray_doc.Root().children[0].Find(L"items"))[0].ends_with(L"汉\U0001F600\b\f"),
              "tray writer preserves decoded path text without malformed JSON");
        app::AppPrefs prefs;
        check(prefs.FromJson(LR"({"nested":{"row_height":100,"show_hidden_files":true},"row_height":34,"show_hidden_files":false})") &&
              prefs.row_height == 34 && !prefs.show_hidden_files,
              "prefs read direct members without borrowing nested values");
        check(prefs.FromJson(LR"({"row_height":999999999999999999})") && prefs.row_height == 34 &&
              !prefs.FromJson(LR"({"show_hidden_files":truefalse})"),
              "prefs integer overflow falls back and bad bool token rejects document");
        app::ContextMenuPrefs menus;
        check(menus.FromJson(LR"({"categories":{"software":false},"items":{"key}brace":{"enabled":false}},"seen":[{"key":"key}brace","text":"name}brace","category":"software"}]})") &&
              !menus.software && menus.item_enabled.contains(L"key}brace") && !menus.item_enabled[L"key}brace"],
              "context-menu structured object lookup survives braces in escaped keys and labels");

        const std::wstring legacy_places = LR"({"active_workspace":0,"workspaces":[{"name":"name} ] \"workspaces\"","root":"C:\\legacy","panes":["C:\\legacy"],"views":["details"]}],"tags":[{"id":"legacy","name":"legacy}tag","rgb":0xEF4444,"paths":[]}],"starred_items":[{"path":"C:\\starred","kind":"folder","badge":"badge}end","rgb":0x0078D4}]})";
        const std::wstring legacy_tags = LR"({"tags":[{"id":"legacy","name":"legacy}tag","rgb":0xEF4444,"paths":[]}]})";
        check(WriteUtf8FileAtomic((root / L"places.json").wstring(), legacy_places) &&
              WriteUtf8FileAtomic((root / L"tags.json").wstring(), legacy_tags), "write isolated actual Places hex-color legacy data");
        {
            model::PlacesCatalog places;
            check(places.Load() && places.workspaces.size() == 1 && places.tags.size() == 1 &&
                  places.tags[0].rgb == 0xEF4444 && places.starred_items.size() == 1 &&
                  places.starred_items[0].badge_rgb == 0x0078D4,
                  "production Places loader preserves old hex colors and braces inside names");
            check(places.Save() && places.FlushPendingSave(true), "save migrated Places values through production writer");
            std::wstring text;
            check(ReadUtf8File((root / L"places.json").wstring(), text) && json::Document(text).Is(json::Kind::Object),
                  "new Places save emits standard JSON numeric colors");
        }
    }
    storage::OverrideDefaultRootForTesting(L"");
    std::error_code error;
    const auto actual = std::filesystem::weakly_canonical(root, error);
    if (error) std::cout << "[INFO] fixture canonical error=" << error.value() << '\n';
    const auto expected_parent = std::filesystem::weakly_canonical(parent, error);
    if (!error && actual.parent_path() == expected_parent &&
        actual.filename().wstring().starts_with(L"json-persistence-")) std::filesystem::remove_all(actual, error);
    else {
        std::wcout << L"[INFO] cleanup root=" << root.wstring() << L" actual=" << actual.wstring()
                   << L" expected parent=" << expected_parent.wstring() << L" error=" << error.value() << L'\n';
        check(false, "isolated fixture cleanup path stayed within bench_data");
    }
    check(!error, "isolated configuration fixture cleaned without user settings");
    return failures ? 1 : 0;
}
