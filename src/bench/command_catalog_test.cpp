#include "../app/command_catalog.h"
#include "../app/context_menu.h"
#include <algorithm>
#include <cstdio>
#include <set>

using namespace pulse;
using namespace pulse::app;

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

namespace {
int passed = 0;
int failed = 0;
void Check(bool value, const char* description) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", description);
    value ? ++passed : ++failed;
}
bool Has(const std::vector<ui::FluentMenuItem>& items, int command) {
    return std::any_of(items.begin(), items.end(), [=](const auto& item) { return item.command == command; });
}
void CheckPalette(const wchar_t* language) {
    l10n::SetLanguage(language);
    constexpr ui::GroupMode modes[] = {ui::GroupMode::Auto, ui::GroupMode::None,
        ui::GroupMode::DateModified, ui::GroupMode::Type, ui::GroupMode::Size};
    for (int mode = 0; mode < 5; ++mode) {
        const auto grouping = BuildGroupingMenu(modes[mode], ui::SortDirection::Desc, true);
        Check(grouping.size() == 6 && grouping[static_cast<size_t>(mode)].radio &&
            std::count_if(grouping.begin(), grouping.end(), [](const auto& item) { return item.radio; }) == 1 &&
            grouping[static_cast<size_t>(mode)].command == CmdGroupAuto + mode,
            "grouping menu preserves selected mode and stable command IDs");
        Check(grouping.back().enabled == (modes[mode] != ui::GroupMode::Auto &&
                modes[mode] != ui::GroupMode::None) &&
            grouping.back().shortcut == l10n::Get(l10n::StringId::GroupDescending) &&
            grouping.back().text == l10n::Get(l10n::StringId::GroupDirection) && !grouping.back().text.empty(),
            "group order has localized direction and is disabled for automatic or no grouping");
    }
    const auto ascending = BuildGroupingMenu(ui::GroupMode::Type, ui::SortDirection::Asc, true);
    Check(ascending.back().shortcut == l10n::Get(l10n::StringId::GroupAscending),
        "ascending group direction is displayed accurately");
    const auto disabled_groups = BuildGroupingMenu(ui::GroupMode::Auto, ui::SortDirection::Desc, false);
    Check(std::none_of(disabled_groups.begin(), disabled_groups.end(), [](const auto& item) { return item.enabled; }),
        "unsupported virtual locations cannot dispatch grouping actions");
    CommandContext context;
    const auto commands = BuildCommandPalette(L">", {}, {}, false, 0, {}, context);
    Check(commands.size() == GetCommandCatalog().size(), "palette includes each catalog command once");
    for (const auto& descriptor : GetCommandCatalog()) {
        const auto found = std::find_if(commands.begin(), commands.end(), [&](const auto& item) {
            return item.command == descriptor.command;
        });
        Check(found != commands.end() && found->text == l10n::Get(descriptor.label) &&
            found->shortcut == CommandShortcutText(descriptor.shortcut) &&
            found->enabled == IsCommandEnabled(descriptor.command, context),
            "palette shares localized name, shortcut and availability");
    }
    const auto rename = l10n::Get(l10n::StringId::Rename);
    const auto filtered = BuildCommandPalette(L">" + rename, {}, {}, false, 0, {}, context);
    Check(Has(filtered, CmdRename) && !Has(filtered, CmdSettings), "command prefix filters localized names");
    const auto settings = BuildCommandPalette(L">" + l10n::Get(l10n::StringId::Settings), {}, {}, false);
    Check(Has(settings, CmdSettings), "settings is discoverable without selection");
}
} // namespace

int main() {
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::set<int> ids;
    for (const auto& descriptor : GetCommandCatalog()) {
        Check(ids.insert(descriptor.command).second, "catalog command IDs are unique");
        if (!descriptor.shortcut.key) continue;
        const auto& key = descriptor.shortcut;
        Check(LookupCommandShortcut(key.key, key.ctrl, key.shift, key.alt) == descriptor.command,
            "shortcut lookup and descriptor agree without collisions");
    }
    Check(LookupCommandShortcut('C', true, false, false) == CmdCopy &&
        LookupCommandShortcut('C', true, true, false) == CmdCopyPath &&
        LookupCommandShortcut('C', true, false, true) == CmdCopyToTarget,
        "copy, copy path and copy to target remain distinct");
    Check(LookupCommandShortcut('C', true, true, true) == CmdNone &&
        LookupCommandShortcut('V', false, false, false) == CmdNone &&
        LookupCommandShortcut(0, false, false, false) == CmdNone,
        "unregistered modifiers, plain editor keys and empty key do not dispatch");
    Check(CommandShortcutText(FindCommandDescriptor(CmdMoveToTarget)->shortcut) == L"Ctrl+Alt+X" &&
        CommandShortcutText(FindCommandDescriptor(CmdProperties)->shortcut) == L"Alt+Enter" &&
        CommandShortcutText(FindCommandDescriptor(CmdRename)->shortcut) == L"F2",
        "shared shortcut captions match established keyboard bindings");

    CommandContext context;
    Check(!IsCommandEnabled(CmdCopy, context) && !IsCommandEnabled(CmdCut, context) &&
        !IsCommandEnabled(CmdRename, context) && !IsCommandEnabled(CmdDelete, context) &&
        !IsCommandEnabled(CmdProperties, context) && !IsCommandEnabled(CmdPaste, context) &&
        !IsCommandEnabled(CmdNewFolder, context), "empty virtual context disables unavailable file operations");
    Check(IsCommandEnabled(CmdSettings, context) && IsCommandEnabled(CmdOperationCenter, context),
        "global commands remain available in empty context");
    context.selected_count = 1;
    context.selection_filesystem = true;
    context.filesystem_folder = true;
    context.has_entries = true;
    context.copy_path_available = true;
    context.terminal_available = true;
    Check(IsCommandEnabled(CmdCopy, context) && IsCommandEnabled(CmdCut, context) &&
        IsCommandEnabled(CmdRename, context) && IsCommandEnabled(CmdDelete, context) &&
        IsCommandEnabled(CmdProperties, context) && IsCommandEnabled(CmdNewFolder, context),
        "normal filesystem selection enables supported operations");
    Check(!IsCommandEnabled(CmdBatchRename, context) && !IsCommandEnabled(CmdPaste, context) &&
        !IsCommandEnabled(CmdCopyToTarget, context), "single selection, empty clipboard and absent target are guarded");
    context.can_paste = true;
    context.has_target = true;
    Check(IsCommandEnabled(CmdPaste, context) && !IsCommandEnabled(CmdMoveToTarget, context),
        "paste uses clipboard capability while unwritable target stays disabled");
    context.target_writable = true;
    context.selected_count = 2;
    Check(IsCommandEnabled(CmdBatchRename, context) && IsCommandEnabled(CmdCopyToTarget, context) &&
        IsCommandEnabled(CmdMoveToTarget, context), "multiple selection and writable target enable batch operations");
    context.read_only = true;
    Check(IsCommandEnabled(CmdCopy, context) && IsCommandEnabled(CmdProperties, context) &&
        IsCommandEnabled(CmdCopyToTarget, context) && !IsCommandEnabled(CmdCut, context) &&
        !IsCommandEnabled(CmdDelete, context) && !IsCommandEnabled(CmdRename, context) &&
        !IsCommandEnabled(CmdPaste, context) && !IsCommandEnabled(CmdMoveToTarget, context) &&
        !IsCommandEnabled(CmdNewFolder, context), "read-only source permits inspection and copy, blocks writes");
    context.read_only = false;
    context.selection_has_drive_root = true;
    Check(!IsCommandEnabled(CmdCopy, context) && !IsCommandEnabled(CmdDelete, context) &&
        !IsCommandEnabled(CmdRename, context) && !IsCommandEnabled(CmdBatchRename, context) &&
        !IsCommandEnabled(CmdCopyToTarget, context), "drive root selection cannot enter transfer or batch rename");
    context.selected_count = 1;
    Check(IsCommandEnabled(CmdRename, context) && IsCommandEnabled(CmdProperties, context),
        "single drive preserves supported label rename and properties");
    context.archive_view = true;
    Check(!IsCommandEnabled(CmdCopy, context) && !IsCommandEnabled(CmdCut, context) &&
        !IsCommandEnabled(CmdDelete, context) && !IsCommandEnabled(CmdRename, context) &&
        !IsCommandEnabled(CmdNewFolder, context) && !IsCommandEnabled(CmdPaste, context) &&
        !IsCommandEnabled(CmdCopyToTarget, context) && !IsCommandEnabled(CmdOpenTerminal, context) &&
        IsCommandEnabled(CmdProperties, context), "archive context disables unsupported actions and retains archive properties");
    Check(!BuildCommandMenuItem(999999, context).enabled &&
        !IsCommandEnabled(999999, context), "unknown commands cannot create enabled blank actions");
    CheckPalette(L"zh-CN");
    CheckPalette(L"en-US");

    const std::vector<std::wstring> recents{L"C:\\A", L"C:\\A", L"C:\\B"};
    const auto recent_items = BuildCommandPalette(L"", recents, {}, false, 0, L"C:\\B");
    Check(Has(recent_items, CmdRecentBase) && !Has(recent_items, CmdRecentBase + 1) &&
        !Has(recent_items, CmdRecentBase + 2), "recent paths still deduplicate and omit current path");
    const auto search_items = BuildCommandPalette(L"?needle", {}, {}, false);
    const auto project_items = BuildCommandPalette(L"", {}, {}, true);
    const auto path_items = BuildCommandPalette(L"C:\\A", {L"C:\\A"}, {}, false);
    Check(!Has(search_items, CmdSettings) && !Has(project_items, CmdSettings) &&
        !Has(path_items, CmdSettings) && Has(path_items, CmdRecentBase),
        "search, project and filesystem path modes preserve their existing behavior");
    index::Hit hit;
    hit.name = L"needle.txt";
    hit.path = L"C:\\needle.txt";
    const auto hits = BuildCommandPalette(L"?needle", {}, {hit}, false, 1);
    Check(Has(hits, CmdIndexBase) && Has(hits, CmdSearchAll), "search still returns index hits and all-results command");
    const auto command_hits = BuildCommandPalette(L">", {L"C:\\A"}, {hit}, false, 1);
    Check(!Has(command_hits, CmdIndexBase) && Has(command_hits, CmdRecentBase),
        "command mode preserves recent folders and suppresses index hits");
    std::printf("%d PASS, %d FAIL\n", passed, failed);
    return failed ? 1 : 0;
}
