#pragma once
#include "../ipc/ctx_menu_util.h"

bool RunContextMenuRefreshTests() {
    using namespace pulse::app;
    bool passed = true;
    ContextMenuPrefs prefs;
    prefs.persist = false;
    ContextMenuController controller;
    uint32_t next_token = 1, invoked_token = 0, invoked_id = 0, queries = 0, closes = 0;
    std::wstring executed_path, executed_verb;
    ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { ++queries; return next_token; };
    operations.close = [&](uint32_t) { ++closes; };
    operations.invoke = [&](uint32_t token, uint32_t id, auto, auto) { invoked_token = token; invoked_id = id; };
    operations.execute_verb = [&](const auto& path, const auto& verb) { executed_path = path; executed_verb = verb; };
    controller.SetShellOperations(std::move(operations));
    pulse::ui::FluentMenuItem create;
    create.command = CmdNewMenu; create.text = L"New"; create.live_update = true;
    for (const auto& [command, text] : std::vector<std::pair<int, std::wstring>>{
        {CmdNewFolder, L"Folder"}, {CmdNewFile, L"File"}, {CmdNewTextFile, L"Text document"}}) {
        pulse::ui::FluentMenuItem child; child.command = command; child.text = text;
        create.children.push_back(std::move(child));
    }
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true, pulse::ipc::kBackgroundVerbKey, false, {}, {});
    controller.OpenMenu({create});
    bool changed = false;
    auto pending = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("pending native templates leave every Pulse creation command usable", pending.front().enabled &&
        pending.front().children.size() == 3 && pending.front().children.front().enabled);
    passed &= Report("cold background menu has no transient loading row", pending.size() == 2 &&
        pending.back().command == CmdSettingsContextMenu);
    pulse::ops::ShellMenuItem parent;
    parent.has_children = true; parent.text = L"Native New"; parent.verb = L"New";
    parent.clsid = L"{D969A300-E7FF-11D0-A93B-00A0C90F2719}"; parent.handler = L"NewMenu";
    auto folder = parent; folder.has_children = false; folder.child = true; folder.id = 11;
    folder.text = L"Localized folder name"; folder.verb = L"NewFolder";
    auto text = folder; text.id = 12; text.text = L"Localized text template"; text.verb = L"New.txt";
    auto office = folder; office.id = 13; office.text = L"Office document"; office.verb = L"New.docx";
    controller.CompleteComQuery(1, {parent, folder, text, office}, GetTickCount64());
    auto live = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("late native templates cannot insert rows into the cold open menu",
        live.size() == pending.size() && live.front().children.size() == 3 &&
        !live.front().children.back().separator_after);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 13, {}, {});
    passed &= Report("a late undisplayed native command is never authorized", invoked_token == 0);
    next_token = 2;
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true, pulse::ipc::kBackgroundVerbKey, false, {}, {});
    controller.CompleteComQuery(2, {parent, folder, text, office}, GetTickCount64());
    controller.OpenMenu({create});
    live = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("native folder and text templates merge by semantic verb without duplicate New headers",
        live.size() == 2 && live.front().children.size() == 4 &&
        live.front().children.back().text == office.text && live.front().children.back().enabled);
    passed &= Report("Pulse creation rows precede the remaining native templates", live.front().children[0].command == CmdNewFolder &&
        live.front().children[1].command == CmdNewFile && live.front().children[2].command == CmdNewTextFile &&
        live.front().children[2].separator_after);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 13, {}, {});
    passed &= Report("merged template invocation keeps the exact displayed COM session", invoked_token == 2 && invoked_id == 13);
    next_token = 3;
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true, pulse::ipc::kBackgroundVerbKey, false, {}, {});
    controller.SeedComItemsFromCache(); controller.OpenMenu({create});
    pending = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("cached native templates remain disabled until validated while builtins remain enabled",
        pending.front().children.size() == 4 && pending.front().children.front().enabled && !pending.front().children.back().enabled);
    office.id = 23;
    controller.CompleteComQuery(3, {parent, folder, text, office}, GetTickCount64());
    live = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("live native template remaps a cached command id before enabling", live.front().children.back().enabled &&
        live.front().children.back().command == CmdShellComBase + 23);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 23, {}, {});
    passed &= Report("remapped native template invokes only the current session", invoked_token == 3 && invoked_id == 23);
    next_token = 4;
    StaticVerb app; app.verb = L"__openwith"; app.display = L"Example editor"; app.app_path = L"C:\\Editor.exe";
    StaticVerb openas; openas.verb = L"openas"; openas.display = pulse::l10n::Get(pulse::l10n::StringId::ShellOpenWith);
    controller.CompleteStaticVerbs(L".txt", {app, openas});
    controller.StartQuery(prefs, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
    controller.CompleteComQuery(4, {}, GetTickCount64()); controller.OpenMenu({});
    live = controller.BuildDisplay(prefs, {}, changed);
    passed &= Report("Open With retains one chooser entrance alongside registered applications",
        live.size() == 3 && live[0].text == app.display && live[1].text == openas.display);
    StaticVerb replacement; replacement.verb = L"replacement"; replacement.display = L"New association";
    passed &= Report("registry refresh is cached without replacing open static command bindings",
        !controller.CompleteStaticVerbs(L".txt", {replacement, app, openas}));
    const auto static_live = controller.BuildDisplay(prefs, {}, changed);
    passed &= Report("late registry entries keep the current menu captions and row count",
        static_live.size() == live.size() && static_live[0].text == live[0].text &&
        static_live[1].text == live[1].text);
    controller.CloseMenu(); controller.ExecuteShellCommand(CmdShellStaticBase + pulse::ipc::kStaticVerbStride, {}, {});
    passed &= Report("single chooser entrance delegates through the existing openas shell operation",
        executed_path == L"C:\\one.txt" && executed_verb == L"openas");
    next_token = 5;
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true,
        pulse::ipc::kBackgroundVerbKey, false, {}, {}, 7);
    controller.CompleteComQuery(5, {parent, folder, text, office}, GetTickCount64());
    controller.OpenMenu({create}, true);
    auto dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("prepared toolbar New menu contains enabled templates without a loading footer",
        dropdown.size() == 4 && dropdown.back().enabled && dropdown.back().text == office.text);
    controller.CloseMenu();
    const auto prepared_queries = queries, prepared_closes = closes;
    controller.ExecuteShellCommand(CmdNone, {}, {});
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true,
        pulse::ipc::kBackgroundVerbKey, false, {}, {}, 7);
    controller.OpenMenu({create});
    live = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("dismissed toolbar and background menus reuse the same prepared directory session",
        queries == prepared_queries && closes == prepared_closes && live.size() == 2 &&
        live.front().children.size() == dropdown.size() && live.front().children.back().enabled);
    controller.CloseMenu();
    controller.ExecuteShellCommand(CmdNewFolder, {}, {});
    next_token = 6;
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true,
        pulse::ipc::kBackgroundVerbKey, false, {}, {}, 7);
    passed &= Report("builtin creation commands do not discard a prepared background session",
        queries == prepared_queries && closes == prepared_closes);
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true,
        pulse::ipc::kBackgroundVerbKey, false, {}, {}, 8);
    passed &= Report("same-path navigation generation retires the prepared session",
        queries == prepared_queries + 1 && closes == prepared_closes + 1 &&
        !controller.CompleteComQuery(5, {parent, office}, GetTickCount64()).accepted);
    controller.SeedComItemsFromCache(); controller.OpenMenu({create}, true);
    dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("new-generation cached toolbar commands are disabled until current validation",
        dropdown.size() == 4 && dropdown.front().enabled && !dropdown.back().enabled);
    office.id = 63;
    controller.CompleteComQuery(6, {parent, folder, text, office}, GetTickCount64(), true);
    auto partial_dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("partial replies bind existing toolbar rows without changing their structure",
        partial_dropdown.size() == dropdown.size() && partial_dropdown.back().enabled &&
        partial_dropdown.back().text == dropdown.back().text &&
        partial_dropdown.back().command == CmdShellComBase + 63);
    auto archive = office; archive.id = 64; archive.verb = L"New.zip"; archive.text = L"Archive template";
    controller.CompleteComQuery(6, {parent, folder, text, office, archive}, GetTickCount64());
    auto completed_dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("final replies cannot append a late template to an open toolbar menu",
        completed_dropdown.size() == dropdown.size() && completed_dropdown.back().text == office.text);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdNone, {}, {});
    controller.OpenMenu({create}, true);
    completed_dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("next open incorporates the newly prepared template",
        completed_dropdown.size() == 5 && completed_dropdown.back().text == archive.text &&
        completed_dropdown.back().enabled);
    controller.CloseMenu();
    next_token = 7;
    controller.StartQuery(prefs, nullptr, {L"C:\\folder"}, true,
        pulse::ipc::kBackgroundVerbKey, true, {}, {}, 8);
    passed &= Report("extended options do not reuse a normal prepared session",
        !controller.CompleteComQuery(6, {parent, office}, GetTickCount64()).accepted);
    controller.InvalidateCaches();
    passed &= Report("configuration invalidation retires live commands and cached template presentation",
        !controller.CompleteComQuery(7, {parent, office}, GetTickCount64()).accepted &&
        !controller.SeedComItemsFromCache());
    next_token = 8;
    controller.StartQuery(prefs, nullptr, {L"C:\\other"}, true,
        pulse::ipc::kBackgroundVerbKey, false, {}, {}, 9);
    controller.OpenMenu({create}, true);
    auto cold_dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    controller.CompleteComQuery(8, {parent, office}, GetTickCount64());
    auto late_dropdown = controller.BuildDisplay(prefs, controller.base_items(), changed);
    passed &= Report("cold toolbar New retains exactly its three usable builtins after late loading",
        cold_dropdown.size() == 3 && late_dropdown.size() == 3 &&
        late_dropdown[0].enabled && late_dropdown[1].enabled && late_dropdown[2].enabled &&
        !late_dropdown[2].separator_after);
    controller.CloseMenu(); controller.Close();
    passed &= Report("native new handler identity is case insensitive and never matched by caption",
        pulse::ipc::IsNativeNewMenuHandler(parent.clsid) && !pulse::ipc::IsNativeNewMenuHandler(L"NewMenu"));
    passed &= Report("template filtering recognizes canonical folder and text verbs but retains other formats",
        pulse::ipc::IsPulseNewTemplateVerb(L"NEWFOLDER") && pulse::ipc::IsPulseNewTemplateVerb(L"New.txt") &&
        !pulse::ipc::IsPulseNewTemplateVerb(L"New.docx") && !pulse::ipc::IsPulseNewTemplateVerb(L"Localized text template"));
    return passed;
}
