// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/places.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

inline int RunWorkspaceSnapshotChecks(const std::wstring& data_dir) {
    using namespace pulse;
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    app::PlacesCatalog places;
    places.persist = false;
    const std::wstring root = L"C:\\Projects\\pulse";
    const auto normalized = fs::NormalizePath(root);
    const std::vector<std::wstring> paths{L"C:\\Projects", L"C:\\Other", L"\\\\server\\share", L"C:\\Fourth"};
    const std::vector<ui::ViewMode> views{ui::ViewMode::Details, ui::ViewMode::LargeIcons,
        ui::ViewMode::SmallIcons, ui::ViewMode::Details};
    for (int focus = 0; focus < 4; ++focus) {
        const int index = places.PinWorkspace(root, L"Pulse", 4, paths, views, focus);
        const auto& workspace = places.workspaces[static_cast<size_t>(index)];
        check(places.workspaces.size() == 1 && workspace.focused_pane == focus &&
              workspace.pane_paths[static_cast<size_t>(focus)] == normalized,
              "pinning a selected folder anchors the focused pane and retains one workspace identity");
        bool others_preserved = true;
        for (size_t pane = 0; pane < paths.size(); ++pane)
            if (pane != static_cast<size_t>(focus)) others_preserved &= workspace.pane_paths[pane] == paths[pane];
        check(others_preserved && workspace.pane_views == views && workspace.layout == 4,
              "capturing a workspace preserves all other pane paths, views and the split preset");
    }
    places.PinWorkspace(root, L"Pulse", 1, {L"C:\\Other", root},
        {ui::ViewMode::Details, ui::ViewMode::LargeIcons}, 1);
    check(places.workspaces[0].focused_pane == 1 && places.workspaces[0].pane_paths[0] == L"C:\\Other" &&
          places.workspaces[0].pane_paths[1] == normalized,
          "pinning the current directory keeps the current split scene");
    const auto before = places.workspaces[0];
    check(places.RenameWorkspace(L"c:\\projects\\PULSE", L"  电气 & AI 项目  ") &&
          places.workspaces[0].name == L"电气 & AI 项目", "workspace renaming trims the label and resolves case-insensitive root identity");
    const auto& renamed = places.workspaces[0];
    check(renamed.root == before.root && renamed.layout == before.layout &&
          renamed.pane_paths == before.pane_paths && renamed.pane_views == before.pane_views &&
          renamed.focused_pane == before.focused_pane && places.active_workspace == 0,
          "renaming changes only the display name, retaining the folder, snapshot and active selection");
    for (const auto& invalid : {std::wstring(L" \t\r\n"), std::wstring(L"bad\nname"), std::wstring(129, L'x')})
        check(!places.RenameWorkspace(root, invalid) && places.workspaces[0].name == L"电气 & AI 项目",
              "invalid workspace names cannot erase or replace the current label");
    check(!places.RenameWorkspace(L"C:\\missing", L"Missing") && places.workspaces.size() == 1,
          "renaming a removed target cannot rename a different workspace");
    places.persist = true;
    check(places.Save(), "persist focused-pane snapshot and renamed workspace in an isolated profile");
    {
        app::PlacesCatalog loaded;
        loaded.persist = false;
        check(loaded.Load() && loaded.workspaces.size() == 1 && loaded.workspaces[0].name == renamed.name &&
              loaded.workspaces[0].focused_pane == 1 && loaded.workspaces[0].pane_paths == renamed.pane_paths &&
              loaded.workspaces[0].pane_views == renamed.pane_views,
              "workspace name, focused pane and split snapshot survive restart");
    }
    check(places.RenameWorkspace(root, L"保存后的新名称") && places.FlushPendingSave(true),
          "workspace renaming uses the existing background persistence queue and can flush successfully");
    {
        app::PlacesCatalog loaded;
        loaded.persist = false;
        check(loaded.Load() && loaded.workspaces[0].name == L"保存后的新名称",
              "renaming an already saved workspace survives restart");
    }
    places.persist = false;
    const auto file = (std::filesystem::path(data_dir) / L"places.json").wstring();
    const auto legacy = LR"({"workspaces":[
        {"name":"pulse","root":"C:\\Projects\\pulse","panes":["C:\\Projects"],"views":["details"]},
        {"name":"Thesis","root":"C:\\Projects\\Thesis","panes":["C:\\Projects"],"views":["details"]},
        {"name":"Camp","root":"C:\\Projects\\Camp","panes":["C:\\Projects"],"views":["details"]},
        {"name":"Split","root":"C:\\Projects\\Split","layout":1,"panes":["C:\\Other","C:\\Projects"],"views":["details","large-icons"]},
        {"name":"Existing","root":"C:\\Projects\\Existing","layout":1,"panes":["C:\\Other","C:\\Projects\\Existing"],"views":["details","large-icons"]},
        {"name":"Explicit","root":"C:\\Projects\\Explicit","layout":1,"focused_pane":1,"panes":["C:\\Other","C:\\Projects\\Explicit\\child"],"views":["details","large-icons"]}
    ]})";
    check(WriteUtf8FileAtomic(file, legacy), "write representative legacy workspace snapshots into the isolated profile");
    app::PlacesCatalog legacy_places;
    legacy_places.persist = false;
    check(legacy_places.Load() && legacy_places.workspaces.size() == 6, "load legacy snapshots without dropping workspace entries");
    if (legacy_places.workspaces.size() == 6) {
        for (size_t i = 0; i < 3; ++i)
            check(legacy_places.workspaces[i].pane_paths[0] == fs::NormalizePath(legacy_places.workspaces[i].root),
                  "old sibling-folder pins recover their own project root instead of the shared parent");
        const auto& split = legacy_places.workspaces[3];
        check(split.focused_pane == 1 && split.pane_paths[0] == L"C:\\Other" &&
              split.pane_paths[1] == fs::NormalizePath(split.root) && split.pane_views[1] == ui::ViewMode::LargeIcons,
              "legacy split repair retains the other pane and the focused view mode");
        check(legacy_places.workspaces[4].focused_pane == 1 &&
              legacy_places.workspaces[4].pane_paths[0] == L"C:\\Other",
              "a legacy scene already containing its root infers focus without rewriting other paths");
        check(legacy_places.workspaces[5].focused_pane == 1 &&
              legacy_places.workspaces[5].pane_paths[1] == L"C:\\Projects\\Explicit\\child",
              "an explicit current snapshot retains its saved focused directory");
        legacy_places.persist = true;
        check(legacy_places.Save(), "persist repaired legacy focus once through the normal writer");
        app::PlacesCatalog reloaded;
        reloaded.persist = false;
        check(reloaded.Load() && reloaded.workspaces.size() == 6 &&
              reloaded.workspaces[3].focused_pane == 1 && reloaded.workspaces[3].pane_paths == split.pane_paths,
              "legacy migration remains stable over another save and restart");
    }
    app::PlacesCatalog empty;
    empty.persist = false;
    empty.PinWorkspace(root, L"", 0, {}, {}, 99);
    check(empty.workspaces[0].pane_paths == std::vector<std::wstring>{normalized} &&
          empty.workspaces[0].focused_pane == 0, "an empty capture still opens the root in a valid focused pane");
    std::printf("WORKSPACES checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
