// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

void RunSevenZipPrefs() {
    using namespace pulse;
    const std::wstring clsid = L"{23170F69-40C1-278A-1000-000100020000}";
    const auto key = ipc::HandlerCatalogKey(clsid);
    app::ContextMenuPrefs loaded; loaded.persist = false;
    Check(loaded.FromJson(LR"({"version":2,"slow_ext":{"h:{23170f69-40c1-278a-1000-000100020000}":{"ms":2200,"slow":3,"timeout":3,"deferred":true,"disabled":false}}})") &&
        loaded.HandlerEnabled(clsid) && loaded.disabled_com.empty(),
        "successful slow-query history does not create an automatic extension policy");
    const auto cleaned = loaded.ToJson();
    Check(cleaned.find(L"\"deferred\"") == std::wstring::npos &&
        cleaned.find(L"\"timeout\"") == std::wstring::npos && cleaned.find(L"\"ms\"") == std::wstring::npos,
        "saving preferences drops obsolete timing and deferral state");
    const std::wstring legacy = LR"({"version":1,"categories":{"software":true},"items":{"f:7-zip":{"enabled":true}},"slow_ext":{"h:{23170f69-40c1-278a-1000-000100020000}":{"ms":1000,"timeout":3,"deferred":true,"disabled":true}}})";
    Check(loaded.FromJson(legacy) && loaded.HandlerEnabled(clsid) && !loaded.ComDisabled(key) &&
        loaded.disabled_com.empty() && loaded.DisabledHandlerClsids().empty(),
        "legacy automatic 7-Zip skip is recovered in memory without modifying the source file");
    app::ContextMenuPrefs reloaded; reloaded.persist = false;
    Check(reloaded.FromJson(loaded.ToJson()) && reloaded.HandlerEnabled(clsid) && !reloaded.ComDisabled(key),
        "migrated automatic skip stays recovered on the next preference load");
    loaded.SetItemEnabled(key, false);
    Check(!loaded.HandlerEnabled(clsid) && loaded.DisabledHandlerClsids().size() == 1,
        "explicitly disabled 7-Zip item still suppresses the handler");
    Check(reloaded.FromJson(loaded.ToJson()) && !reloaded.HandlerEnabled(clsid),
        "explicit 7-Zip item disable survives preference roundtrip");
    loaded.SetItemEnabled(key, true);
    loaded.SetComDisabled(key, true);
    Check(reloaded.FromJson(loaded.ToJson()) && reloaded.ComDisabled(key) && !reloaded.HandlerEnabled(clsid),
        "current explicit slow-handler disable survives preference roundtrip");
    Check(reloaded.FromJson(LR"({"version":1,"slow_ext":{"h:{23170f69-40c1-278a-1000-000100020000}":{"timeout":0,"disabled":true}}})") &&
        reloaded.ComDisabled(key), "legacy disable without automatic-threshold evidence remains explicit");
    Check(reloaded.FromJson(LR"({"version":1,"items":{"h:{23170f69-40c1-278a-1000-000100020000}":{"enabled":false}},"slow_ext":{"h:{23170f69-40c1-278a-1000-000100020000}":{"timeout":3,"disabled":true}}})") &&
        !reloaded.HandlerEnabled(clsid) && !reloaded.DisabledHandlerClsids().empty(),
        "legacy automatic recovery never overrides an explicit item disable");
}

void RunSevenZip(HWND owner, pulse::ui::Compositor& compositor,
                 const std::filesystem::path& output) {
    using namespace pulse;
    const auto find_vendor = [](const auto& rows) -> const ui::FluentMenuItem* {
        for (const auto& row : rows) if (row.text == L"7-Zip") return &row;
        return nullptr;
    };
    const auto find_more = [](const auto& rows) -> const ui::FluentMenuItem* {
        for (const auto& row : rows) if (row.glyph == L"\xE712") return &row;
        return nullptr;
    };
    std::filesystem::create_directories(output);
    for (const auto& selection : std::vector<std::vector<std::wstring>>{
        {L"C:\\fixture\\one.txt"}, {L"C:\\fixture\\one.txt", L"C:\\fixture\\two.txt"},
        {L"C:\\fixture\\one.txt", L"C:\\fixture\\archive.zip"}}) {
        app::ContextMenuPrefs prefs; prefs.persist = false;
        app::ContextMenuController controller;
        uint32_t token = 1, invoked_token = 0, invoked_id = 0;
        std::vector<std::wstring> queried;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](auto paths, HWND, bool, bool, auto) { queried = paths; return token; };
        operations.close = [](uint32_t) {};
        operations.invoke = [&](uint32_t session, uint32_t id, auto, auto) {
            invoked_token = session; invoked_id = id;
        };
        controller.SetShellOperations(std::move(operations));
        ui::FluentMenuItem open; open.command = app::CmdOpen; open.text = L"Open";
        ui::FluentMenuItem properties; properties.command = app::CmdProperties; properties.text = L"Properties";
        const std::vector<ui::FluentMenuItem> base{open, properties};
        controller.StartQuery(prefs, owner, selection, false, selection.size() == 1 ? L".txt" : L":file", false, {}, {});
        Check(queried == selection, "7-Zip query keeps every single or multiple selected path");
        controller.OpenMenu(base);
        bool changed = false;
        const auto pending = controller.BuildDisplay(prefs, base, changed);
        ui::FluentMenu menu;
        Check(menu.Create(owner, &compositor, 1), "create actual menu for delayed 7-Zip query");
        ui::FluentMenuTestPeer::Begin(menu, pending);
        ops::ShellMenuItem parent; parent.has_children = true; parent.text = L"7-Zip";
        parent.clsid = L"{23170F69-40C1-278A-1000-000100020000}"; parent.handler = L"7-Zip Shell Extension";
        auto leaf = parent; leaf.has_children = false; leaf.child = true; leaf.id = 17;
        leaf.text = L"Add to archive..."; leaf.verb = L"SevenZipCompress";
        auto other = leaf; other.id = 300; other.child = false; other.clsid = L"{other}";
        other.text = L"Other software"; other.verb = L"other.inspect";
        controller.CompleteComQuery(token, {parent, leaf, other}, GetTickCount64(), true);
        auto display = controller.BuildDisplay(prefs, base, changed);
        const auto* vendor = find_vendor(display);
        Check(vendor && vendor->enabled && vendor->command == 0 && vendor->children.size() == 1 &&
            vendor->children.front().command == app::CmdShellComBase + 17,
            "single multi and mixed selection expose an enabled top-level 7-Zip submenu");
        const auto* more = find_more(display);
        Check(more && more->children.size() == 1 && more->children.front().text == other.text,
            "7-Zip is not duplicated under More options and other software remains accessible");
        Check(menu.ReplaceItems(display), "actual open popup accepts late top-level 7-Zip without moving builtins");
        controller.NotePatchedDisplay();
        auto second = leaf; second.id = 18; second.text = L"Compress and email..."; second.verb = L"SevenZipEmail";
        controller.CompleteComQuery(token, {parent, leaf, second, other}, GetTickCount64());
        display = controller.BuildDisplay(prefs, base, changed); vendor = find_vendor(display);
        Check(vendor && vendor->children.size() == 2 && menu.ReplaceItems(display),
            "final Shell reply refreshes every 7-Zip command in the open popup");
        controller.NotePatchedDisplay();
        const auto label = selection.size() == 1 ? L"single" : selection.back().ends_with(L".zip") ? L"mixed" : L"multi";
        if (vendor) {
            for (const auto scale : {1.0f, 1.5f}) for (const auto dark : {false, true}) {
                ui::FluentMenu shot;
                Check(shot.Create(owner, &compositor, scale), "create 7-Zip visual verification popup");
                shot.SetTheme(dark, D2D1::ColorF(0, .47f, .84f));
                const auto stem = std::wstring(label) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    (dark ? L"-dark" : L"-light");
                Check(shot.SaveDebugSnapshot((output / (stem + L".png")).c_str(), display, 2) &&
                    ui::FluentMenuTestPeer::LabelsFit(shot), "top-level 7-Zip text fits tested DPI and theme");
                Check(shot.SaveDebugSnapshot((output / (stem + L"-children.png")).c_str(), vendor->children, 0) &&
                    ui::FluentMenuTestPeer::LabelsFit(shot), "7-Zip submenu commands fit tested DPI and theme");
            }
        }
        menu.Dismiss(); controller.CloseMenu();
        controller.ExecuteShellCommand(app::CmdShellComBase + 18, {}, {});
        Check(invoked_token == 1 && invoked_id == 18, "promoted 7-Zip leaf retains exact displayed session and command");
        token = 2;
        controller.StartQuery(prefs, owner, selection, false, selection.size() == 1 ? L".txt" : L":file", false, {}, {});
        Check(controller.SeedComItemsFromCache(), "complete 7-Zip reply seeds the next selection query");
        controller.OpenMenu(base); display = controller.BuildDisplay(prefs, base, changed); vendor = find_vendor(display);
        Check(vendor && !vendor->enabled && !vendor->children.front().enabled,
            "cached 7-Zip submenu cannot invoke the previous selection session");
        leaf.id = 27; second.id = 28;
        controller.CompleteComQuery(token, {parent, leaf, second}, GetTickCount64());
        display = controller.BuildDisplay(prefs, base, changed); vendor = find_vendor(display);
        Check(vendor && vendor->enabled && vendor->children.front().command == app::CmdShellComBase + 27,
            "live 7-Zip query validates cache and remaps selection-specific command IDs");
        prefs.SetItemEnabled(ipc::HandlerCatalogKey(parent.clsid), false);
        Check(!find_vendor(controller.BuildDisplay(prefs, base, changed)), "explicit 7-Zip handler preference remains effective");
        prefs.SetItemEnabled(ipc::HandlerCatalogKey(parent.clsid), true);
        controller.NotePatchedDisplay(); controller.CloseMenu();
        controller.ExecuteShellCommand(app::CmdShellComBase + 27, {}, {});
        Check(invoked_token == 2 && invoked_id == 27, "validated cached 7-Zip invokes only the current selection session");
    }
}
