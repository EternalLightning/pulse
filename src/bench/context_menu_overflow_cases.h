// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

bool RunContextMenuOverflowTests() {
    using namespace pulse::app;
    bool passed = true;
    ContextMenuPrefs prefs;
    prefs.persist = false;
    ContextMenuController controller;
    uint32_t token = 1, invoked_token = 0, invoked_id = 0, closed = 0;
    ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { return token; };
    operations.close = [&](uint32_t) { ++closed; };
    operations.invoke = [&](uint32_t session, uint32_t id, auto, auto) {
        invoked_token = session; invoked_id = id;
    };
    controller.SetShellOperations(std::move(operations));
    pulse::ui::FluentMenuItem builtin;
    builtin.command = CmdOpen; builtin.text = L"Open";
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture.txt"}, false, L".txt", false, {}, {});
    controller.OpenMenu({builtin});
    bool changed = false;
    const auto pending = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("cold Shell query reserves one overflow row after builtins",
        pending.size() == 2 && pending.front().command == CmdOpen &&
        pending.back().command != CmdSettingsContextMenu && ShellSection(pending).size() == 1);
    passed &= Report("loading is non-invokable and confined to the overflow flyout",
        !ShellSection(pending).front().enabled && !ShellSection(pending).front().command);

    pulse::ops::ShellMenuItem parent;
    parent.text = L"Vendor tools"; parent.has_children = true;
    parent.clsid = L"{fixture-a}"; parent.handler = L"Fixture vendor";
    auto child = parent;
    child.has_children = false; child.child = true; child.id = 21;
    child.text = L"Inspect fixture"; child.verb = L"fixture.inspect";
    controller.CompleteComQuery(1, {parent, child}, GetTickCount64(), true);
    auto partial = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("first late vendor reply replaces the placeholder without growing the main menu",
        partial.size() == pending.size() && ShellSection(partial).size() == 1 &&
        ShellSection(partial).front().text == parent.text);
    passed &= Report("overflow preserves vendor hierarchy and its exact enabled command",
        ShellSection(partial).front().children.size() == 1 &&
        ShellSection(partial).front().children.front().command == CmdShellComBase + 21 &&
        ShellSection(partial).front().children.front().enabled);
    auto late = child;
    late.id = 22; late.verb = L"fixture.late"; late.text = L"Late vendor command";
    auto flat = child;
    flat.child = false; flat.id = 257; flat.clsid = L"{fixture-b}";
    flat.handler = L"Other fixture"; flat.verb = L"fixture.other"; flat.text = L"Other action";
    controller.CompleteComQuery(1, {parent, child, late, flat}, GetTickCount64());
    const auto final = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("final reply admits late vendor children and flat verbs inside the reserved overflow",
        final.size() == pending.size() && ShellSection(final).size() == 2 &&
        ShellSection(final).front().children.size() == 2 &&
        ShellSection(final).back().command == CmdShellComBase + 257);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 22, {}, {});
    passed &= Report("nested late selection invokes only the displayed current session",
        invoked_token == 1 && invoked_id == 22);

    token = 2;
    controller.StartQuery(prefs, nullptr, {L"C:\\other.txt"}, false, L".txt", false, {}, {});
    controller.SeedComItemsFromCache(); controller.OpenMenu({builtin});
    const auto cached = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("cached nested vendor commands stay disabled before validation",
        ShellSection(cached).size() == 2 && !ShellSection(cached).front().enabled &&
        !ShellSection(cached).front().children.front().enabled);
    child.id = 31; late.id = 32;
    controller.CompleteComQuery(2, {parent, child, late, flat}, GetTickCount64());
    const auto remapped = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("current validation remaps nested ids while preserving main and vendor captions",
        remapped.size() == cached.size() && ShellSection(remapped).front().text == parent.text &&
        ShellSection(remapped).front().children.front().command == CmdShellComBase + 31 &&
        ShellSection(remapped).front().children.front().enabled);
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 31, {}, {});
    passed &= Report("validated nested cache invokes its new id and session", invoked_token == 2 && invoked_id == 31);

    token = 3;
    controller.StartQuery(prefs, nullptr, {L"C:\\empty.unknown"}, false, L".unknown", false, {}, {});
    controller.OpenMenu({builtin});
    controller.CompleteComQuery(3, {}, GetTickCount64());
    const auto empty = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("completed empty Shell response retains only the builtins",
        empty.size() == 1 && ShellSection(empty).empty() && empty.front().command == CmdOpen);
    controller.CloseMenu(); controller.Close();
    passed &= Report("empty session is closed through the existing lifecycle", closed != 0);

    token = 4;
    controller.StartQuery(prefs, nullptr, {L"C:\\many.fixture"}, false, L".fixture", false, {}, {});
    std::vector<pulse::ops::ShellMenuItem> many;
    for (uint32_t i = 1; i <= 60; ++i) {
        auto item = flat; item.id = i; item.text = L"Vendor action " + std::to_wstring(i);
        item.verb = L"fixture." + std::to_wstring(i); many.push_back(std::move(item));
    }
    controller.CompleteComQuery(4, many, GetTickCount64()); controller.OpenMenu({builtin});
    const auto crowded = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("sixty extensions leave the main menu at builtin plus overflow",
        crowded.size() == 2 && crowded[0].command == CmdOpen && crowded[1].live_update);
    passed &= Report("overflow respects the existing extension safety cap", ShellSection(crowded).size() == 48);
    prefs.item_enabled[pulse::ipc::HandlerCatalogKey(flat.clsid)] = false;
    const auto filtered = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("existing handler preferences still filter overflow contents", ShellSection(filtered).empty());
    controller.CloseMenu(); controller.Close();

    token = 5;
    prefs.item_enabled.clear();
    controller.StartQuery(prefs, nullptr, {L"C:\\unpatched.fixture"}, false, L".unpatched", false, {}, {});
    controller.OpenMenu({builtin});
    controller.CompleteComQuery(5, {flat}, GetTickCount64());
    controller.BuildDisplay(prefs, {builtin}, changed);
    controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 257, {}, {});
    passed &= Report("building a late subtree without publishing it cannot authorize a command",
        invoked_token == 2 && invoked_id == 31);
    token = 6;
    controller.StartQuery(prefs, nullptr, {L"C:\\groups.fixture"}, false, L".groups", false, {}, {});
    controller.OpenMenu({builtin});
    auto second_parent = parent;
    second_parent.text = L"Other vendor submenu"; second_parent.verb = L"fixture.submenu";
    auto same_child = child;
    same_child.id = 301;
    controller.CompleteComQuery(6, {parent, child, second_parent, same_child}, GetTickCount64());
    const auto groups = controller.BuildDisplay(prefs, {builtin}, changed);
    passed &= Report("late sibling flyouts retain equal-caption children within their own parent",
        ShellSection(groups).size() == 2 && ShellSection(groups)[0].children.size() == 1 &&
        ShellSection(groups)[1].children.size() == 1 &&
        ShellSection(groups)[0].children[0].command == CmdShellComBase + 31 &&
        ShellSection(groups)[1].children[0].command == CmdShellComBase + 301);
    controller.CloseMenu(); controller.Close();
    return passed;
}
