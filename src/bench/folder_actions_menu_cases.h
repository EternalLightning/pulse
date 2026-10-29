#pragma once
#include "../app/app_folder_actions.h"
#include "../app/context_menu.h"

template<class Check>
void RunFolderActionsMenuChecks(Check check) {
    using namespace pulse;
    app::Tab tab;
    tab.current_path = L"C:\\fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*entries)[0].name = L"folder"; (*entries)[0].is_dir = true;
    (*entries)[1].name = L"file.txt";
    tab.snapshot = entries;
    tab.SelectOnly(0);
    check(!CanGroupSelection(&tab) && CanDissolveSelection(&tab), "one plain folder offers dissolve only");
    tab.SelectOnly(1);
    check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "one file offers neither folder action");
    tab.SelectRange(0, 1);
    check(CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "mixed real children offer grouping only");
    (*entries)[1].is_dir = true;
    check(CanGroupSelection(&tab) && CanDissolveSelection(&tab), "several plain folders offer both actions");
    (*entries)[0].is_reparse = true;
    check(CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "directory links can move as entries but cannot dissolve");
    (*entries)[0].is_reparse = false;
    (*entries)[0].change_record_only = true;
    check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "historical change rows disable folder actions");
    (*entries)[0].change_record_only = false;
    (*entries)[0].full_path = L"C:\\other\\folder";
    check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "selection outside the current parent is refused");
    (*entries)[0].full_path.clear();
    tab.net_readonly = true;
    check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "read-only locations disable both actions");
    tab.net_readonly = false;
    for (const auto* virtual_path : {L"pulse:search:", L"pulse:recycle", L"pulse:archive:sample", L"pulse:device:phone"}) {
        tab.current_path = virtual_path;
        check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "virtual views do not offer filesystem folder actions");
    }
    tab.current_path = L"\\\\server";
    check(!CanGroupSelection(&tab) && !CanDissolveSelection(&tab), "UNC server share lists cannot be grouped or dissolved");
    tab.current_path = L"\\\\server\\share";
    check(CanGroupSelection(&tab) && CanDissolveSelection(&tab), "writable UNC share folders support both actions");
    app::CommandContext context;
    context.selected_count = 2; context.selection_filesystem = true; context.filesystem_folder = true;
    context.can_group_selection = context.can_dissolve_selection = true;
    check(app::IsCommandEnabled(app::CmdNewFolderWithSelection, context) &&
        app::IsCommandEnabled(app::CmdUngroupFolder, context), "command catalog enables eligible folder actions");
    context.device_selection = true;
    check(!app::IsCommandEnabled(app::CmdNewFolderWithSelection, context) &&
        !app::IsCommandEnabled(app::CmdUngroupFolder, context), "command dispatch capabilities refuse device selections");
    context.device_selection = false; context.selection_has_drive_root = true;
    check(!app::IsCommandEnabled(app::CmdNewFolderWithSelection, context) &&
        !app::IsCommandEnabled(app::CmdUngroupFolder, context), "command dispatch capabilities refuse drive roots");
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    check(l10n::Get(l10n::StringId::NewFolderWithSelection) == L"合并到文件夹" &&
        l10n::Get(l10n::StringId::UngroupFolder) == L"解散文件夹", "folder action commands have Chinese labels");
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    check(l10n::Get(l10n::StringId::NewFolderWithSelection) == L"New Folder with Selection" &&
        l10n::Get(l10n::StringId::UngroupFolder) == L"Dissolve Folders", "folder action commands have English labels");
}
