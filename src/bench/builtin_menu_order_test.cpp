#include "../app/builtin_menu_items.h"
#include "../app/context_menu.h"
#include "../app/context_menu_prefs.h"
#include "../common/localization.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
} // namespace pulse::app

namespace {
using namespace pulse;
using B = app::BuiltinMenuItem;
using S = app::BuiltinMenuSurface;

ui::FluentMenuItem Item(int command) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = L"fixture";
    return item;
}

bool HasCommand(const std::vector<ui::FluentMenuItem>& items, int command) {
    for (const auto& item : items) {
        if (item.command == command || HasCommand(item.children, command)) return true;
        for (const auto& swatch : item.quick_swatches)
            if (swatch.command == command) return true;
    }
    return false;
}

app::BuiltinMenuOrder MoveFirst(S surface, B item) {
    auto order = app::BuiltinMenuDefaultOrder(surface);
    std::erase(order, item);
    order.insert(order.begin(), item);
    return order;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    static_assert(static_cast<int>(B::OpenInNewTab) == 0);
    static_assert(static_cast<int>(B::RowMore) == 12);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };

    if (argc == 2 && std::wstring_view(argv[1]) == L"--legacy-builtin") {
        constexpr const wchar_t* legacy = L"pulse:quick-access";
        constexpr const wchar_t* legacy_json = LR"({"items":{"pulse:quick-access":{"enabled":false}},"seen":[{"key":"pulse:quick-access","text":"固定到快速访问","kind":"verb","category":"software"}]})";
        app::ContextMenuPrefs prefs;
        prefs.persist = false;
        check(prefs.FromJson(legacy_json) && !prefs.BuiltinItemEnabled(B::QuickAccess) &&
              prefs.seen.empty() && prefs.item_enabled.empty(),
              "legacy private visibility migrates to the builtin switch without a ghost catalog row");
        check(!prefs.ItemEnabled(legacy, ipc::CtxMenuCategory::Software, false) &&
              !prefs.CatalogItemVisible(legacy) && prefs.ComDisabled(legacy) &&
              prefs.HiddenCatalogKeys().empty(),
              "all legacy identity readers use the builtin switch without publishing a Shell disable key");
        const auto normalized = prefs.ToJson();
        app::ContextMenuPrefs roundtrip;
        roundtrip.persist = false;
        check(normalized.find(legacy) == std::wstring::npos && roundtrip.FromJson(normalized) &&
              !roundtrip.BuiltinItemEnabled(B::QuickAccess) && roundtrip.seen.empty(),
              "migrated choice roundtrips solely through pulse_items");
        roundtrip.MigrateSeenKeys();
        roundtrip.MigrateSeenKeys();
        check(roundtrip.ToJson() == normalized, "catalog migration is idempotent");
        check(prefs.FromJson(LR"({"pulse_items":{"quick_access":true},"items":{"pulse:quick-access":{"enabled":false}},"slow_ext":{"pulse:quick-access":{"disabled":true}}})") &&
              prefs.BuiltinItemEnabled(B::QuickAccess) && prefs.item_enabled.empty() &&
              prefs.disabled_com.empty(), "explicit new enabled choice overrides both legacy disables");
        check(prefs.FromJson(LR"({"pulse_items":{"quick_access":false},"items":{"pulse:quick-access":{"enabled":true}}})") &&
              !prefs.BuiltinItemEnabled(B::QuickAccess), "explicit new disabled choice overrides legacy enable");
        check(prefs.FromJson(LR"({"pulse_items":{"copy_path":false},"items":{"pulse:quick-access":{"enabled":false}}})") &&
              !prefs.BuiltinItemEnabled(B::QuickAccess) && !prefs.BuiltinItemEnabled(B::CopyPath),
              "a partial new document still migrates an unspecified builtin choice");
        check(prefs.FromJson(LR"({"pulse_items":{"quick_access":"invalid"},"items":{"pulse:quick-access":{"enabled":false}}})") &&
              !prefs.BuiltinItemEnabled(B::QuickAccess), "malformed new value does not erase a valid legacy choice");
        check(prefs.FromJson(LR"({"slow_ext":{"pulse:quick-access":{"disabled":true}}})") &&
              !prefs.BuiltinItemEnabled(B::QuickAccess) && prefs.disabled_com.empty(),
              "explicit legacy disable migrates without retaining a second control");
        check(prefs.FromJson(LR"({"items":{"pulse:quick-access":{"enabled":true}},"slow_ext":{"pulse:quick-access":{"disabled":true}}})") &&
              prefs.BuiltinItemEnabled(B::QuickAccess), "explicit legacy item selection takes priority over its slow_ext record");
        check(prefs.FromJson(LR"({"version":1,"slow_ext":{"pulse:quick-access":{"disabled":true,"timeout":3}}})") &&
              prefs.BuiltinItemEnabled(B::QuickAccess), "obsolete automatic timeouts are not migrated as user hides");
        check(!prefs.RecordSeen(legacy, L"Pin to Quick access", false, ipc::CtxMenuCategory::Software) &&
              prefs.seen.empty(), "private builtin cannot reenter the observed Shell catalog");
        prefs.SetItemEnabled(legacy, false);
        check(!prefs.BuiltinItemEnabled(B::QuickAccess) && prefs.item_enabled.empty(),
              "legacy item setter routes to the builtin switch");
        prefs.SetComDisabled(legacy, false);
        check(prefs.BuiltinItemEnabled(B::QuickAccess) && prefs.disabled_com.empty(),
              "legacy disable setter also routes to the builtin switch");
        prefs.SetComDisabled(legacy, true);
        check(!prefs.BuiltinItemEnabled(B::QuickAccess) && prefs.HiddenCatalogKeys().empty(),
              "legacy disable cannot create a Shell synchronization override");
        const auto shell_key = ipc::CatalogKey(L"固定到快速访问", false);
        const auto handler = ipc::HandlerCatalogKey(L"{11111111-2222-3333-4444-555555555555}");
        prefs.RecordSeen(shell_key, L"固定到快速访问", false, ipc::CtxMenuCategory::Software, true);
        prefs.RecordSeen(handler, L"固定到快速访问", false, ipc::CtxMenuCategory::Software, true);
        prefs.SetItemEnabled(shell_key, false);
        prefs.SetComDisabled(handler, true);
        check(roundtrip.FromJson(prefs.ToJson()) && roundtrip.seen.size() == 2 &&
              !roundtrip.CatalogItemVisible(shell_key) && roundtrip.ComDisabled(handler) &&
              !roundtrip.BuiltinItemEnabled(B::QuickAccess),
              "same-caption Shell command and handler preserve independent identity and choices");
        check(roundtrip.FromJson(LR"({"seen":[{"key":"v:固定到快速访问","text":"固定到快速访问","kind":"verb","source":"com"}],"items":{"pulse:quick-access":{"enabled":false},"v:固定到快速访问":{"enabled":true}}})") &&
              roundtrip.seen.size() == 1 && roundtrip.CatalogItemVisible(shell_key) &&
              !roundtrip.BuiltinItemEnabled(B::QuickAccess),
              "caption-only rows remain intact when lost provenance cannot be safely recovered");
        const auto retired = app::BuiltinMenuBit(B::NewFolder) | app::BuiltinMenuBit(B::NewTextFile);
        check(prefs.FromJson(LR"({"pulse_items":{"new_folder":false,"new_text":false}})") &&
              !prefs.BuiltinItemEnabled(B::NewMenu) && !(prefs.builtin_hidden & retired),
              "legacy all-hidden creation children migrate to the current New group switch");
        check(prefs.FromJson(LR"({"pulse_items":{"new_menu":true,"new_folder":false,"new_text":false}})") &&
              prefs.BuiltinItemEnabled(B::NewMenu) && prefs.BuiltinItemEnabled(B::NewFolder) &&
              prefs.BuiltinItemEnabled(B::NewTextFile) && !(prefs.builtin_hidden & retired),
              "explicit current New group enable overrides retired hidden children");
        check(prefs.FromJson(LR"({"pulse_items":{"new_menu":false,"new_folder":true,"new_text":true}})") &&
              !prefs.BuiltinItemEnabled(B::NewMenu) && !(prefs.builtin_hidden & retired),
              "explicit current New group hide overrides retired enabled children");
        check(prefs.FromJson(LR"({"pulse_items":{"new_folder":false,"new_text":true}})") &&
              prefs.BuiltinItemEnabled(B::NewMenu) && !(prefs.builtin_hidden & retired),
              "partial legacy creation selection upgrades to an editable visible New group");
        check(prefs.FromJson(LR"({"pulse_items":{"new_folder":false}})") &&
              prefs.BuiltinItemEnabled(B::NewMenu), "missing legacy children do not hide the current New group");
        prefs.SetBuiltinItemEnabled(B::NewFolder, false);
        check(!prefs.BuiltinItemEnabled(B::NewMenu) && !(prefs.builtin_hidden & retired),
              "retired New folder setter aliases the current group without a hidden child bit");
        prefs.SetBuiltinItemEnabled(B::NewTextFile, true);
        check(prefs.BuiltinItemEnabled(B::NewMenu) && prefs.BuiltinItemEnabled(B::NewFolder),
              "retired New text setter and readers also share the current group");
        prefs.SetBuiltinItemEnabled(B::NewMenu, false);
        const auto grouped_json = prefs.ToJson();
        bool stable_grouped = true;
        for (int pass = 0; pass < 3; ++pass) {
            stable_grouped &= roundtrip.FromJson(pass ? roundtrip.ToJson() : grouped_json) &&
                !roundtrip.BuiltinItemEnabled(B::NewMenu) && !(roundtrip.builtin_hidden & retired) &&
                roundtrip.ToJson() == grouped_json;
        }
        check(stable_grouped, "grouped creation visibility remains stable across repeated JSON roundtrips");
        prefs.SetBuiltinGroupEnabled(false);
        check(!prefs.BuiltinGroupEnabled() && !(prefs.builtin_hidden & retired) &&
              roundtrip.FromJson(prefs.ToJson()) && !roundtrip.BuiltinGroupEnabled(),
              "all builtin group switches stay off after retiring child bits and reloading");
        return failures ? 1 : 0;
    }

    constexpr std::array old_keys{L"open_new_tab", L"copy_path", L"terminal", L"quick_access",
        L"pin_workspace", L"pin_network", L"tags", L"recent_changes", L"select", L"undo",
        L"row_new_tab", L"row_star", L"row_more"};
    bool stable_keys = true;
    std::set<std::wstring> keys;
    for (int index = 0; index < app::kBuiltinMenuItemCount; ++index) {
        const auto item = static_cast<B>(index);
        keys.emplace(app::BuiltinMenuKey(item));
        if (index < static_cast<int>(old_keys.size()))
            stable_keys &= app::BuiltinMenuKey(item) == old_keys[index];
    }
    check(stable_keys && keys.size() == static_cast<size_t>(app::kBuiltinMenuItemCount) &&
          !keys.contains(L""), "old enum keys stay stable and appended keys remain distinct");
    check(app::BuiltinMenuKey(B::Count).empty() && app::BuiltinMenuBit(B::Count) == 0 &&
          !app::BuiltinMenuShared(B::Count) && app::BuiltinMenuSurfaceRows(S::Count).count == 0 &&
          app::BuiltinMenuOrderedRows(S::Count, {}).empty(), "invalid model values have safe empty results");

    bool presets = true;
    for (int index = 0; index < static_cast<int>(app::BuiltinMenuPreset::Count); ++index) {
        const auto preset = static_cast<app::BuiltinMenuPreset>(index);
        presets &= app::BuiltinMenuPresetFor(app::BuiltinMenuPresetHidden(preset)) == preset;
    }
    const auto slim = app::BuiltinMenuPresetHidden(app::BuiltinMenuPreset::Slim);
    check(presets && !(slim & app::BuiltinMenuBit(B::NewMenu)) &&
          (slim & app::BuiltinMenuBit(B::Tags)) &&
          app::BuiltinMenuPresetFor(app::BuiltinMenuBit(B::Tags)) == app::BuiltinMenuPreset::Count,
          "presets roundtrip and the slim preset retains the local New flyout");

    const auto background = app::BuiltinMenuDefaultOrder(S::Background);
    check(background[0] == B::View && background[1] == B::RecentChanges &&
          std::count(background.begin(), background.end(), B::NewMenu) == 1 &&
          std::find(background.begin(), background.end(), B::NewFolder) == background.end() &&
          std::find(background.begin(), background.end(), B::NewTextFile) == background.end(),
          "New child commands are excluded from the movable background surface");
    const auto cleaned = app::NormalizeBuiltinMenuOrder(S::Background,
        {B::Paste, B::Paste, B::Tags, B::NewFolder, B::Count, B::View});
    std::set<B> normalized_set(cleaned.begin(), cleaned.end());
    check(cleaned.size() == background.size() && normalized_set.size() == background.size() &&
          std::find(cleaned.begin(), cleaned.end(), B::Tags) == cleaned.end() &&
          std::find(cleaned.begin(), cleaned.end(), B::NewFolder) == cleaned.end(),
          "normalization drops duplicates and foreign rows and restores missing defaults");

    const auto undo_first = MoveFirst(S::Item, B::Undo);
    const auto table = app::BuiltinMenuSurfaceRows(S::Item);
    const auto moved_rows = app::BuiltinMenuOrderedRows(S::Item, undo_first);
    bool separators = moved_rows.size() == table.count;
    for (size_t index = 0; index < moved_rows.size(); ++index) {
        separators &= moved_rows[index].separator_after == table.rows[index].separator_after;
        if (table.rows[index].fixed != app::BuiltinFixedRow::None)
            separators &= moved_rows[index].fixed == table.rows[index].fixed;
    }
    check(separators && moved_rows[2].item == B::Undo,
          "movable rows keep fixed rows and the separators of their destination slots");
    const auto all_hidden = app::BuiltinMenuVisibleRows(S::Item, app::kBuiltinMenuMask, undo_first);
    check(all_hidden.size() == 2 && all_hidden[0].row.fixed == app::BuiltinFixedRow::Strip &&
          all_hidden[1].row.fixed == app::BuiltinFixedRow::Open && !all_hidden.back().separator_after,
          "hiding all configurable rows retains fixed actions without a trailing separator");
    check(app::BuiltinMenuShared(B::CopyPath) && app::BuiltinMenuShared(B::Terminal) &&
          !app::BuiltinMenuShared(B::NewMenu), "shared visibility only covers real shared commands");

    const app::BuiltinMenuOrder row_order{B::RowMore, B::RowStar, B::RowNewTab};
    check(app::RowActionMask(0, row_order) == (7u | ((2u | (1u << 4)) << 3)) &&
          app::RowActionMask(app::BuiltinMenuBit(B::RowStar), row_order) ==
              (6u | ((2u | (1u << 4)) << 3)) &&
          app::RowActionMask(0, app::BuiltinMenuDefaultOrder(S::RowButtons)) == 7u &&
          app::RowActionMask(0, {B::Count, B::Tags}) == 7u,
          "row order encoding preserves low visibility bits and normalizes invalid input");

    app::ContextMenuPrefs prefs;
    prefs.persist = false;
    prefs.SetBuiltinOrder(S::Item, undo_first);
    prefs.SetBuiltinOrder(S::RowButtons, row_order);
    prefs.SetBuiltinVisible(B::NewTextFile, false);
    const auto shell_key = ipc::CatalogKey(L"Third-party fixture", false);
    prefs.SetItemEnabled(shell_key, false);
    prefs.RecordSeen(shell_key, L"Third-party fixture", false, ipc::CtxMenuCategory::Software, true);
    app::ContextMenuPrefs reread;
    reread.persist = false;
    check(reread.FromJson(prefs.ToJson()) && reread.BuiltinOrder(S::Item) == undo_first &&
          reread.BuiltinOrder(S::RowButtons) == row_order && !reread.BuiltinOrderCustom(S::Background) &&
          !reread.BuiltinVisible(B::NewTextFile) && reread.seen.size() == 1 &&
          !reread.ItemEnabled(shell_key, ipc::CtxMenuCategory::Software, true),
          "per-surface order roundtrip retains builtin visibility and independent Explorer prefs");
    reread.SetBuiltinOrder(S::Item, app::BuiltinMenuDefaultOrder(S::Item));
    check(!reread.BuiltinOrderCustom(S::Item) && reread.builtin_order[0].empty(),
          "default orders collapse to an empty stored override");
    check(reread.FromJson(LR"({"pulse_items":{"copy_path":false,"row_more":false},"pulse_order":{"item":"undo,undo,row_star,unknown_future_key","background":false}})") &&
          reread.builtin_hidden == (app::BuiltinMenuBit(B::CopyPath) | app::BuiltinMenuBit(B::RowMore)) &&
          reread.BuiltinOrder(S::Item).size() == app::BuiltinMenuDefaultOrder(S::Item).size() &&
          !reread.BuiltinOrderCustom(S::Background) && reread.BuiltinVisible(B::Properties) &&
          reread.BuiltinVisible(B::NewMenu), "old and malformed JSON keeps appended commands visible and normalizes orders");
    check(reread.FromJson(L"{}") && reread.builtin_hidden == 0 && !reread.BuiltinOrderCustom(S::Item) &&
          !reread.BuiltinOrderCustom(S::RowButtons), "loading a legacy file clears stale builtin order overrides");
    reread.SetBuiltinOrder(S::Background, {B::Paste});
    reread.SetBuiltinVisible(B::Tags, false);
    reread.ResetToDefaults();
    check(reread.builtin_hidden == 0 && !reread.BuiltinOrderCustom(S::Background),
          "reset defaults clears both visibility and order overrides");

    auto strip = Item(app::CmdNone);
    for (const auto command : {app::CmdCut, app::CmdCopy, app::CmdDelete, app::CmdRename})
        strip.quick_swatches.push_back({command});
    const int open_with = app::CmdShellStaticBase + 9;
    const int shell_command = app::CmdShellComBase + 17;
    auto copy = Item(app::CmdCopyPath);
    copy.separator_after = true;
    auto tag_strip = Item(app::CmdTags);
    tag_strip.quick_swatches.push_back({app::CmdTagBase});
    auto tag_picker = Item(app::CmdTags);
    tag_picker.separator_after = true;
    const auto unchanged = app::ApplyBuiltinMenuPrefs({Item(app::CmdUndo), Item(app::CmdCopyPath)}, 0);
    const auto visibility_only = app::ApplyBuiltinMenuPrefs({Item(app::CmdUndo), Item(app::CmdCopyPath)},
        app::BuiltinMenuBit(B::Tags));
    check(unchanged[0].command == app::CmdUndo && visibility_only[0].command == app::CmdUndo,
          "an omitted order preserves builder order for default and visibility-only preferences");
    const auto real_ordered = app::ApplyBuiltinMenuPrefs({strip, Item(app::CmdOpen), Item(open_with),
        copy, tag_strip, tag_picker, Item(shell_command), Item(app::CmdUndo), Item(app::CmdProperties)},
        0, {B::Undo, B::Tags, B::Properties, B::CopyPath});
    check(real_ordered.size() == 9 && real_ordered[0].quick_swatches.size() == 4 &&
          real_ordered[1].command == app::CmdOpen && real_ordered[2].command == open_with &&
          real_ordered[3].command == app::CmdUndo && real_ordered[3].separator_after &&
          real_ordered[4].command == app::CmdTags && !real_ordered[4].quick_swatches.empty() &&
          real_ordered[5].command == app::CmdTags && real_ordered[5].separator_after &&
          real_ordered[6].command == shell_command && real_ordered[7].command == app::CmdProperties &&
          real_ordered[8].command == app::CmdCopyPath && !real_ordered.back().separator_after,
          "real menus move tag blocks with slot separators and retain Open with and Shell slots");
    const auto real_hidden = app::ApplyBuiltinMenuPrefs(real_ordered, app::kBuiltinMenuMask, undo_first);
    check(real_hidden.size() == 4 && HasCommand(real_hidden, app::CmdOpen) && HasCommand(real_hidden, app::CmdCut) &&
          HasCommand(real_hidden, open_with) && HasCommand(real_hidden, shell_command) &&
          !HasCommand(real_hidden, app::CmdProperties) && !HasCommand(real_hidden, app::CmdTags),
          "all optional rows can hide while fixed actions and extension commands remain");

    auto create = Item(app::CmdNewMenu);
    create.children = {Item(app::CmdNewFolder), Item(app::CmdNewFile), Item(app::CmdNewTextFile)};
    const auto old_new_hidden = app::ApplyBuiltinMenuPrefs({create}, app::BuiltinMenuBit(B::NewFolder) |
        app::BuiltinMenuBit(B::NewTextFile), background);
    check(old_new_hidden.size() == 1 && old_new_hidden[0].command == app::CmdNewMenu &&
          old_new_hidden[0].children.size() == 3 &&
          app::ApplyBuiltinMenuPrefs({create}, app::BuiltinMenuBit(B::NewMenu), background).empty(),
          "New flyout has one visible group switch and ignores retired child bits");
    auto group = Item(app::CmdNone);
    group.glyph = L"\xF168";
    group.children = {Item(app::CmdGroupAuto), Item(app::CmdGroupNone)};
    auto shell_group = Item(app::CmdNone);
    shell_group.glyph = L"\xF168";
    shell_group.children = {Item(shell_command)};
    const auto groups = app::ApplyBuiltinMenuPrefs({group, shell_group}, app::BuiltinMenuBit(B::Group));
    check(groups.size() == 1 && HasCommand(groups, shell_command) && !HasCommand(groups, app::CmdGroupAuto),
          "group headers use local child command ids without hiding similar Shell flyouts");

    app::ContextMenuPrefs catalog;
    catalog.persist = false;
    const auto share_key = ipc::CatalogKey(L"Share fixture", false);
    const auto disabled_key = ipc::HandlerCatalogKey(L"{00000000-0000-0000-0000-000000000001}");
    catalog.RecordSeen(share_key, L"Share fixture", false, ipc::CtxMenuCategory::Share, true);
    catalog.SetItemEnabled(shell_key, false);
    catalog.SetComDisabled(disabled_key, true);
    catalog.SetItemEnabled(disabled_key, false);
    const auto hidden_catalog = catalog.HiddenCatalogKeys();
    check(hidden_catalog.size() == 3 && std::is_sorted(hidden_catalog.begin(), hidden_catalog.end()) &&
          !catalog.CatalogItemVisible(share_key) && !catalog.CatalogItemVisible(disabled_key),
          "system-menu synchronization receives sorted unique hidden catalog keys");
    catalog.SetItemEnabled(disabled_key, true);
    check(catalog.CatalogItemVisible(disabled_key) && !catalog.ComDisabled(disabled_key),
          "enabling a catalog item clears its explicit COM disable");

    std::printf("Builtin menu order failures: %d\n", failures);
    return failures ? 1 : 0;
}
