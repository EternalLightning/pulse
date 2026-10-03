#include "../app/builtin_menu_items.h"
#include "../app/context_menu.h"
#include "../app/context_menu_prefs.h"
#include "../common/localization.h"
#include "../ipc/ctx_menu_util.h"
#include <cstdio>
#include <set>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
} // namespace pulse::app

namespace {
pulse::ui::FluentMenuItem Item(int command, const wchar_t* text = L"fixture") {
    pulse::ui::FluentMenuItem item;
    item.command = command;
    item.text = text;
    return item;
}
bool HasCommand(const std::vector<pulse::ui::FluentMenuItem>& items, int command) {
    for (const auto& item : items) {
        if (item.command == command) return true;
        for (const auto& swatch : item.quick_swatches)
            if (swatch.command == command) return true;
        if (HasCommand(item.children, command)) return true;
    }
    return false;
}
} // namespace

int wmain() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    app::ContextMenuPrefs prefs;
    prefs.persist = false;
    std::set<std::wstring> keys;
    for (int index = 0; index < app::kBuiltinMenuItemCount; ++index)
        keys.emplace(app::BuiltinMenuKey(static_cast<app::BuiltinMenuItem>(index)));
    check(keys.size() == static_cast<size_t>(app::kBuiltinMenuItemCount) && !keys.contains(L""),
          "every builtin setting has a distinct stable persistence key");
    check(prefs.BuiltinGroupEnabled() && prefs.builtin_hidden == 0,
          "new preferences show every Pulse command and row action");
    prefs.SetBuiltinItemEnabled(app::BuiltinMenuItem::Tags, false);
    prefs.SetBuiltinItemEnabled(app::BuiltinMenuItem::RowMore, false);
    const std::wstring shell_key = ipc::CatalogKey(L"Third-party fixture", false);
    prefs.SetItemEnabled(shell_key, false);
    prefs.RecordSeen(shell_key, L"Third-party fixture", false,
                     ipc::CtxMenuCategory::Software, true);
    app::ContextMenuPrefs loaded;
    loaded.persist = false;
    check(loaded.FromJson(prefs.ToJson()) && loaded.builtin_hidden == prefs.builtin_hidden &&
          loaded.seen.size() == 1 && loaded.seen[0].key == shell_key &&
          !loaded.ItemEnabled(shell_key, ipc::CtxMenuCategory::Software, true),
          "keyed builtin roundtrip preserves independent Explorer overrides and seen ordering");
    check(loaded.FromJson(LR"({"pulse_items":{"unknown_future_key":false,"copy_path":false}})") &&
          loaded.builtin_hidden == app::BuiltinMenuBit(app::BuiltinMenuItem::CopyPath),
          "unknown persisted items do not hide unrelated commands");
    check(loaded.FromJson(L"{}") && loaded.builtin_hidden == 0,
          "legacy files without Pulse preferences restore visible defaults");
    loaded.SetBuiltinGroupEnabled(false);
    check(!loaded.BuiltinGroupEnabled() && app::RowActionMask(loaded.builtin_hidden) == 0,
          "the appended Pulse group toggle hides all registered items");
    loaded.SetBuiltinItemEnabled(app::BuiltinMenuItem::RowNewTab, true);
    check(loaded.BuiltinGroupEnabled() && app::RowActionMask(loaded.builtin_hidden) == 2u,
          "individual row actions can be restored from a disabled group");
    loaded.ResetToDefaults();
    check(loaded.builtin_hidden == 0 && app::RowActionMask(loaded.builtin_hidden) == 7u,
          "restore defaults removes all builtin visibility overrides");
    for (uint32_t mask = 0; mask < 8; ++mask) {
        uint32_t hidden = 0;
        if (!(mask & 1u)) hidden |= app::BuiltinMenuBit(app::BuiltinMenuItem::RowStar);
        if (!(mask & 2u)) hidden |= app::BuiltinMenuBit(app::BuiltinMenuItem::RowNewTab);
        if (!(mask & 4u)) hidden |= app::BuiltinMenuBit(app::BuiltinMenuItem::RowMore);
        check(app::RowActionMask(hidden) == mask,
              "row visibility bits independently preserve each enabled button");
    }

    auto core_strip = Item(app::CmdNone, L"");
    for (const auto command : {app::CmdCut, app::CmdCopy, app::CmdDelete, app::CmdRename}) {
        ui::FluentMenuSwatch swatch;
        swatch.command = command;
        core_strip.quick_swatches.push_back(swatch);
    }
    auto tags = Item(app::CmdTags);
    tags.quick_swatches.push_back({app::CmdTagBase});
    auto empty_group = Item(app::CmdNone);
    empty_group.children = {Item(app::CmdSelectAll), Item(app::CmdInvertSelection)};
    auto shell_group = Item(app::CmdNone);
    shell_group.children = {Item(app::CmdOpenTerminal), Item(app::CmdShellComBase + 17)};
    auto copied_path = Item(app::CmdCopyPath);
    copied_path.separator_after = true;
    auto undo = Item(app::CmdUndo);
    undo.separator_after = true;
    const auto visible = app::FilterBuiltinMenuItems({Item(app::CmdOpen), copied_path,
        core_strip, tags, empty_group, shell_group, Item(app::CmdProperties), undo},
        app::kBuiltinMenuMask);
    check(visible.size() == 4 && visible[0].command == app::CmdOpen &&
          visible[0].separator_after && visible[1].quick_swatches.size() == 4 &&
          visible[2].children.size() == 1 && visible[2].children[0].command == app::CmdShellComBase + 17 &&
          visible.back().command == app::CmdProperties && !visible.back().separator_after,
          "hidden rows transfer separators, remove empty flyouts and tag strips, and retain Shell children");
    check(HasCommand(visible, app::CmdOpen) && HasCommand(visible, app::CmdCut) &&
          HasCommand(visible, app::CmdCopy) && HasCommand(visible, app::CmdDelete) &&
          HasCommand(visible, app::CmdRename) && HasCommand(visible, app::CmdProperties) &&
          !HasCommand(visible, app::CmdTags) && !HasCommand(visible, app::CmdSelectAll) &&
          !HasCommand(visible, app::CmdUndo),
          "mandatory operations remain available when every optional Pulse menu item is hidden");
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        bool labels = true;
        for (int index = 0; index < app::kBuiltinMenuItemCount; ++index)
            labels &= !app::BuiltinMenuLabel(static_cast<app::BuiltinMenuItem>(index)).empty();
        check(labels, "builtin settings expose labels in each supported UI language");
    }
    std::printf("Builtin menu failures: %d\n", failures);
    return failures ? 1 : 0;
}
