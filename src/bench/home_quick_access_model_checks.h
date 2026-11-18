// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../app/app_prefs.h"
#include "../app/home_catalog.h"
#include "../app/app_model.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include "../fs/shell_namespace.h"
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <functional>

namespace pulse::bench {
inline void RunHomeQuickAccessModelChecks(const std::filesystem::path& root,
    const std::function<void(bool, const char*)>& check) {
    namespace sysfs = std::filesystem;
    model::HomeQuickAccessPrefs prefs;
    check(prefs.hidden == 0 && prefs.folders.empty(), "home quick access defaults retain all ten built-in cards");
    const auto first = (root / L"quick-access-first").wstring();
    const auto second = (root / L"quick-access-second").wstring();
    sysfs::create_directories(first);
    sysfs::create_directories(second);
    check(prefs.AddFolder(first) && prefs.AddFolder(second), "home custom folders add in user order");
    std::wstring same = first + L"\\";
    CharUpperBuffW(same.data(), static_cast<DWORD>(same.size()));
    check(!prefs.AddFolder(same) && !prefs.AddFolder(L"\\\\?\\" + first),
        "home custom folder identity ignores case, trailing slash and extended drive prefix");
    check(!prefs.AddFolder(L"relative\\folder") && !prefs.AddFolder(L"C:relative") &&
        !prefs.AddFolder(L"\\\\server") && !prefs.AddFolder(L"C:\\folder:stream") &&
        !prefs.AddFolder(L"\\\\.\\PhysicalDrive0") && !prefs.AddFolder(L"pulse:home"),
        "home folder validation rejects relative, incomplete UNC, stream, device and virtual paths without probing disk");
    check(prefs.AddFolder(L"\\\\?\\UNC\\server\\share\\long folder\\") &&
        !prefs.AddFolder(L"\\\\SERVER\\SHARE\\long folder"), "home UNC and extended UNC folders share the same identity");
    check(prefs.RemoveFolder(L"\\\\server\\share\\long folder\\"), "home custom folders remove through equivalent UNC paths");
    check(prefs.MoveFolder(second, -1) && path::EqualInsensitive(prefs.folders.front(), second) &&
        !prefs.MoveFolder(second, -1) && prefs.MoveFolder(second, 1) &&
        !prefs.MoveFolder(second, 1) && !prefs.MoveFolder(first, 0),
        "home folder reorder respects direction and first/last boundaries");
    check(prefs.RemoveFolder(first + L"\\") && !prefs.RemoveFolder(first), "home folder removal is stable and reports a missing item");

    app::AppPrefs app_prefs;
    check(app_prefs.FromJson(L"{\"version\":4,\"new_tab_home\":true}") &&
        app_prefs.home_quick_access == model::HomeQuickAccessPrefs{}, "legacy app preferences keep default home quick access");
    app_prefs.home_quick_access = prefs;
    app_prefs.home_quick_access.hidden = model::kHomeQuickAccessHiddenMask;
    const auto file = root / L"home-quick-access-app.json";
    std::wstring saved;
    app::AppPrefs restored;
    check(restored.FromJson(L"{\"home_quick_access\":{\"hidden\":178}}") &&
        restored.home_quick_access.hidden == 178,
        "adding Documents and Music preserves every previously stored visibility bit");
    check(WriteUtf8FileAtomic(file.wstring(), app_prefs.ToJson()) && ReadUtf8File(file.wstring(), saved) &&
        restored.FromJson(saved) && restored.home_quick_access == app_prefs.home_quick_access,
        "home hidden bits and ordered custom folders round-trip through production app preference serialization");
    app_prefs.home_quick_access.folders.clear();
    check(restored.FromJson(app_prefs.ToJson()) && restored.home_quick_access.hidden == model::kHomeQuickAccessHiddenMask &&
        restored.home_quick_access.folders.empty(), "explicitly hiding every home item survives with an empty custom folder list");
    check(restored.FromJson(L"{\"home_quick_access\":{\"hidden\":-1,\"folders\":[7,\"relative\",\"C:\\\\Folder\",\"c:/folder/\"]}}") &&
        restored.home_quick_access.hidden == 0 && restored.home_quick_access.folders.size() == 1,
        "malformed home preference fields sanitize invalid paths and duplicate folders");
    restored.home_quick_access.hidden = model::kHomeQuickAccessHiddenMask;
    restored.ResetToDefaults();
    check(restored.home_quick_access == model::HomeQuickAccessPrefs{}, "reset defaults restores all home quick access cards");

    app::HomeCatalog catalog;
    catalog.Refresh();
    ui::PaneViewModel pane;
    pane.is_home = true;
    auto wait_for = [&](const std::function<bool()>& ready) {
        const auto until = GetTickCount64() + 12000;
        do {
            catalog.Fill(pane);
            if (ready()) return true;
            Sleep(10);
        } while (GetTickCount64() < until);
        return false;
    };
    check(wait_for([&] {
        const auto recycle = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.path == app::MakeRecyclePath(); });
        const auto libraries = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.path == fs::kLibrariesPath; });
        return recycle != pane.home_cards.end() && libraries != pane.home_cards.end() && recycle + 1 == libraries;
    }), "default background home catalog keeps Recycle Bin immediately before Libraries");
    const auto built_in_count = std::count_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.group == 0; });
    for (const auto* id : {&FOLDERID_Documents, &FOLDERID_Music}) {
        PWSTR known_path = nullptr;
        const HRESULT result = SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &known_path);
        check(SUCCEEDED(result) && known_path && std::any_of(pane.home_cards.begin(), pane.home_cards.end(), [&](const auto& card) {
            return card.group == 0 && path::EqualInsensitive(card.path, known_path) &&
                path::EqualInsensitive(card.shell_icon_path, known_path);
        }), "home includes the system-resolved Documents or Music folder with its native icon path");
        CoTaskMemFree(known_path);
    }
    model::HomeQuickAccessPrefs custom;
    custom.hidden = (1u << 5) | (1u << 6) | (1u << 7);
    custom.AddFolder(first);
    custom.AddFolder(second);
    if (!pane.home_cards.empty()) custom.AddFolder(pane.home_cards.front().path);
    catalog.Refresh(custom);
    check(wait_for([&] {
        const auto one = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [&](const auto& card) { return path::EqualInsensitive(card.path, first); });
        const auto two = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [&](const auto& card) { return path::EqualInsensitive(card.path, second); });
        const auto recycle = std::find_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.path == app::MakeRecyclePath(); });
        return one != pane.home_cards.end() && two != pane.home_cards.end() && recycle != pane.home_cards.end() &&
            one + 1 == two && two + 1 == recycle &&
            std::count_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.group == 0; }) == built_in_count - 3 + 2;
    }), "custom home folders deduplicate visible defaults, retain user order before Recycle Bin, and independently hide the three special locations");
    model::HomeQuickAccessPrefs latest;
    latest.hidden = model::kHomeQuickAccessHiddenMask;
    latest.AddFolder(second);
    for (int i = 0; i < 12; ++i) catalog.Refresh(i % 2 ? model::HomeQuickAccessPrefs{} : latest);
    catalog.Refresh(latest);
    check(wait_for([&] {
        return std::count_if(pane.home_cards.begin(), pane.home_cards.end(), [](const auto& card) { return card.group == 0; }) == 1 &&
            std::any_of(pane.home_cards.begin(), pane.home_cards.end(), [&](const auto& card) {
                return card.group == 0 && path::EqualInsensitive(card.path, second) && path::EqualInsensitive(card.shell_icon_path, second);
            });
    }), "rapid home refreshes publish the latest custom folder and hidden special locations with a native icon path");
}
} // namespace pulse::bench
