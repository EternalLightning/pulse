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

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--places-tags-load")
        return RunPlacesTagLoadingCases();
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok;
    };
    {
        app::LayoutTabSnapshot saved;
        saved.panes.push_back({});
        app::LayoutTab restored;
        std::vector<std::wstring> requested;
        const auto load = [&](app::Tab& tab, const std::wstring& path) {
            requested.push_back(path);
            tab.current_path = path;
        };
        app::RestoreLayoutTab(restored, saved, load);
        check(restored.panes.size() == 1 && restored.panes[0]->view.current_path.empty() &&
              requested == std::vector<std::wstring>{L""},
              "session restore preserves This PC as an empty navigation path");
        saved.layout = static_cast<int>(app::LayoutPreset::TwoVertical);
        app::PaneFolderSnapshot folder;
        folder.path = L"C:\\fixture";
        saved.panes.push_back(folder);
        saved.focused = 1;
        requested.clear();
        app::RestoreLayoutTab(restored, saved, load);
        check(restored.panes.size() == 2 && restored.panes[0]->view.current_path.empty() &&
              restored.panes[1]->view.current_path == folder.path && restored.focused_index == 1 &&
              requested == std::vector<std::wstring>{L"", folder.path},
              "session restore keeps This PC and folder panes in their original positions");
    }
    if (argc == 2 && std::string_view(argv[1]) == "--session-this-pc") return failures ? 1 : 0;
    {
        app::WindowTabs tabs;
        const int maximum = (std::numeric_limits<int>::max)();
        app::RestoreWindowTabs(tabs, {}, {{maximum, L"maximum", 0, false},
                                        {1, L"first", 0, false}}, 0, {});
        check(tabs.next_tab_group_id == 1 && app::AllocateTabGroupId(tabs) == 2,
              "session INT_MAX group wraps without colliding with an existing group");
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
    if (argc == 2 && std::string_view(argv[1]) == "--session-group-id") return failures ? 1 : 0;
    const auto parent = std::filesystem::current_path() / L"bench_data";
    const auto root = parent / (L"json-persistence-" + std::to_wstring(GetCurrentProcessId()) +
                               L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting(root.wstring());
    {
        const std::wstring old_session = LR"({"version":7,"left":1,"top":2,"right":800,"bottom":600,"path":"C:\\old","tray":[],"undo":[],"layoutTabs":[{"title":"legacy","panes":[{"path":"C:\\pane","view":"details","cols":"0,0,0"}]}]})";
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(), old_session), "write isolated actual legacy session shape");
        app::SessionSnapshot loaded;
        check(app::LoadSession(loaded) && loaded.window_rect.left == 1 && loaded.layout_tabs.size() == 1 &&
              loaded.layout_tabs[0].title == L"legacy", "legacy session schema and layout panes remain readable");
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(),
              LR"({"child":{"left":999,"path":"fake"},"left":5,"path":"real","tray":[],"undo":[]})") &&
              app::LoadSession(loaded) && loaded.window_rect.left == 5 && loaded.active_path == L"real",
              "production session loader selects root members over nested pseudo-state");
        check(WriteUtf8FileAtomic((root / L"session.json").wstring(), LR"({"left":truejunk})") &&
              !app::LoadSession(loaded) && loaded.window_rect.left == 5,
              "damaged session rejected before replacing current snapshot");
        app::SessionSnapshot original;
        original.active_path = root.wstring();
        app::LayoutTabSnapshot tab;
        tab.title = L"braces } ] \" 汉\U0001F600\b\f";
        app::PaneFolderSnapshot pane;
        pane.path = root.wstring();
        tab.panes.push_back(pane);
        original.layout_tabs.push_back(tab);
        original.tab_groups.push_back({1, L"group } ] \" 汉\U0001F600", 0x123456, true});
        check(app::SaveSession(original) && app::LoadSession(loaded) && loaded.layout_tabs.size() == 1 &&
              loaded.layout_tabs[0].title == tab.title && loaded.tab_groups.size() == 1 &&
              loaded.tab_groups[0].name == original.tab_groups[0].name,
              "session writer/loader round-trip complete escapes and braces in titles");
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
