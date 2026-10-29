#pragma once
#include "../app/context_menu_controller.h"
#include "../app/quick_access_menu.h"
#include <algorithm>
#include <array>
#include <cstdio>

inline int RunBuiltinMenuContractChecks() {
    using namespace pulse;
    using namespace pulse::app;
    using B = BuiltinMenuItem;
    using S = BuiltinMenuSurface;
    int checks = 0, failures = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        failures += !passed;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    };
    const auto command_for = [](B item) -> int {
        switch (item) {
        case B::OpenInNewTab: return CmdOpenInNewTab;
        case B::CopyPath: return CmdCopyPath;
        case B::Terminal: return CmdOpenTerminal;
        case B::QuickAccess: return CmdPinQuickAccess;
        case B::PinWorkspace: return CmdPinWorkspace;
        case B::PinNetwork: return CmdPinNetwork;
        case B::Tags: return CmdTags;
        case B::RecentChanges: return CmdViewRecentChanges;
        case B::SelectCommands: return CmdSelectAll;
        case B::Undo: return CmdUndo;
        case B::View: return CmdViewBase;
        case B::Sort: return CmdSortName;
        case B::Group: return CmdGroupAuto;
        case B::Refresh: return CmdRefresh;
        case B::NewMenu: return CmdNewMenu;
        case B::Paste: return CmdPaste;
        case B::Properties: return CmdProperties;
        case B::FolderProperties: return CmdFolderProperties;
        default: return -1;
        }
    };
    const auto contains = [](const auto& self, const std::vector<ui::FluentMenuItem>& rows, int command) -> bool {
        for (const auto& row : rows) {
            if (row.command == command || self(self, row.children, command)) return true;
            for (const auto& swatch : row.quick_swatches)
                if (swatch.command == command) return true;
        }
        return false;
    };
    const auto menu_order = [&](const std::vector<ui::FluentMenuItem>& rows, S surface) {
        BuiltinMenuOrder order;
        for (const auto& row : rows) {
            for (const auto item : BuiltinMenuDefaultOrder(surface)) {
                if (contains(contains, {row}, command_for(item))) {
                    order.push_back(item);
                    break;
                }
            }
        }
        return order;
    };
    PlacesCatalog places;
    places.persist = false;
    for (const auto surface : {S::Item, S::Background}) {
        ContextMenuPrefs prefs;
        prefs.persist = false;
        auto raw = surface == S::Item ? BuildItemMenu(true, L"Undo fixture", true)
                                     : BuildBackgroundMenu(true, true, L"Undo fixture");
        AppendRecentChangesCommand(raw, L"C:\\fixture", true);
        if (surface == S::Background) {
            BackgroundViewOptions options;
            options.filesystem = true;
            AppendBackgroundViewCommands(raw, options);
        }
        AppendQuickAccessMenuCommand(raw, places, {L"C:\\fixture"});
        ContextMenuController controller;
        controller.StartQuery(prefs, nullptr, {L"C:\\fixture"}, surface == S::Background,
            L":folder", false, {}, {});
        bool changed = false;
        const auto all = controller.BuildDisplay(prefs, raw, changed);
        const auto expected = BuiltinMenuDefaultOrder(surface);
        check(menu_order(all, surface) == expected,
            "real default menu order agrees with the settings model on each surface");
        for (const auto item : expected) {
            const int command = command_for(item);
            check(command > 0 && contains(contains, all, command),
                "each setting offered for the surface has a real menu command");
            prefs.SetBuiltinItemEnabled(item, false);
            const auto hidden = controller.BuildDisplay(prefs, raw, changed);
            check(!contains(contains, hidden, command),
                "each current visibility switch hides its real command or submenu");
            const auto check_children = [&](const auto& self, const ui::FluentMenuItem& row) -> void {
                if (row.command > 0) check(!contains(contains, hidden, row.command),
                    "hiding a group removes every real descendant command");
                for (const auto& swatch : row.quick_swatches)
                    check(!contains(contains, hidden, swatch.command),
                        "hiding a group also removes its quick-action buttons");
                for (const auto& child : row.children) self(self, child);
            };
            for (const auto& row : all)
                if (contains(contains, {row}, command)) check_children(check_children, row);
            for (const auto other : expected)
                if (other != item) check(contains(contains, hidden, command_for(other)),
                    "hiding a setting preserves every other eligible command");
            prefs.SetBuiltinItemEnabled(item, true);
            ContextMenuPrefs loaded;
            loaded.persist = false;
            check(loaded.FromJson(prefs.ToJson()) && contains(contains,
                controller.BuildDisplay(loaded, raw, changed), command),
                "each visibility switch restores its real command after persistence");
        }
        auto reverse = expected;
        std::reverse(reverse.begin(), reverse.end());
        prefs.SetBuiltinOrder(surface, reverse);
        check(menu_order(controller.BuildDisplay(prefs, raw, changed), surface) == reverse,
            "real customized order agrees with the settings model");
        prefs.ResetBuiltinOrder();
        check(menu_order(controller.BuildDisplay(prefs, raw, changed), surface) == expected,
            "restoring the order returns to the same default shown in settings");
        prefs.software = prefs.share = prefs.system_extra = false;
        check(menu_order(controller.BuildDisplay(prefs, raw, changed), surface) == expected,
            "Shell category switches cannot disable Pulse built-in commands");
        for (int preset = 0; preset < static_cast<int>(BuiltinMenuPreset::Count); ++preset) {
            prefs.builtin_hidden = BuiltinMenuPresetHidden(static_cast<BuiltinMenuPreset>(preset));
            const auto display = controller.BuildDisplay(prefs, raw, changed);
            for (const auto item : expected) check(contains(contains, display, command_for(item)) ==
                prefs.BuiltinItemEnabled(item), "each preset applies to all real menu commands");
        }
    }
    ContextMenuPrefs prefs;
    prefs.persist = false;
    std::vector<ui::FluentMenuItem> groups;
    AppendBackgroundViewCommands(groups, {});
    for (auto& row : groups) row.glyph = L"fixture replacement icon";
    for (const auto item : {B::View, B::Sort, B::Group}) {
        prefs.SetBuiltinItemEnabled(item, false);
        check(!contains(contains, ApplyBuiltinMenuPrefs(groups, prefs, S::Background), command_for(item)),
            "submenu ownership depends on command identity instead of its icon");
        prefs.SetBuiltinItemEnabled(item, true);
    }
    ContextMenuController controller;
    ContextMenuController::ShellOperations operations;
    operations.query = [](auto, HWND, bool, bool, auto) { return 77u; };
    controller.SetShellOperations(std::move(operations));
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture"}, true, L":background", false, {}, {});
    ui::FluentMenuItem create;
    create.command = CmdNewMenu;
    create.children = BuildNewMenu();
    controller.OpenMenu({create});
    bool changed = false;
    controller.BuildDisplay(prefs, {create}, changed);
    ops::ShellMenuItem header;
    header.has_children = true;
    header.text = L"New";
    header.clsid = L"{d969a300-e7ff-11d0-a93b-00a0c90f2719}";
    ops::ShellMenuItem child;
    child.id = 17; child.child = true; child.text = L"Office fixture"; child.verb = L".fixture";
    child.clsid = header.clsid;
    ops::ShellMenuItem native_pin;
    native_pin.id = 18; native_pin.verb = L"pintohome"; native_pin.text = L"固定到快速访问";
    ops::ShellMenuItem vendor = native_pin;
    vendor.id = 19; vendor.verb = L"fixture.vendor";
    check(controller.CompleteComQuery(77, {header, child, native_pin, vendor}, GetTickCount64(), false).accepted,
        "accept a late native New template result");
    prefs.SetBuiltinItemEnabled(B::NewMenu, false);
    check(!contains(contains, controller.BuildDisplay(prefs, {create}, changed), CmdNewMenu),
        "late native templates cannot recreate a disabled New submenu");
    prefs.SetBuiltinItemEnabled(B::NewMenu, true);
    const auto restored = controller.BuildDisplay(prefs, {create}, changed);
    check(contains(contains, restored, CmdNewFolder) && contains(contains, restored, CmdNewFile) &&
        contains(contains, restored, CmdNewTextFile) && contains(contains, restored, CmdShellComBase + 17),
        "one New switch restores built-in children and late native templates together");
    auto with_pin = std::vector<ui::FluentMenuItem>{create};
    AppendQuickAccessMenuCommand(with_pin, places, {L"C:\\fixture"});
    prefs.SetBuiltinItemEnabled(B::QuickAccess, false);
    const auto hidden_pin = controller.BuildDisplay(prefs, with_pin, changed);
    check(!contains(contains, hidden_pin, CmdPinQuickAccess) &&
        !contains(contains, hidden_pin, CmdShellComBase + 18) &&
        contains(contains, hidden_pin, CmdShellComBase + 19),
        "hiding Pulse pin also suppresses its native duplicate and retains a same-caption vendor command");
    controller.OpenMenu({create}, true);
    prefs.SetBuiltinItemEnabled(B::NewMenu, false);
    check(contains(contains, controller.BuildDisplay(prefs, {create}, changed), CmdNewFolder),
        "right-click visibility settings leave the toolbar New actions available");
    controller.CloseMenu();
    std::printf("CONTRACT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
