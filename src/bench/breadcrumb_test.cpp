#include "../app/app_model.h"
#include "../common/localization.h"
#include <cstdio>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

int wmain() {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    const auto archive_case = [&](const std::wstring& address, const std::wstring& title) {
        app::Pane pane;
        pane.NewTab(address);
        auto* tab = pane.ActiveTab();
        tab->virtual_title = title;
        ui::PaneViewModel vm;
        app::FillPaneViewModel(vm, pane, nullptr);
        check(vm.path == address, "archive pane preserves the full virtual address");
        const auto crumbs = ui::SplitBreadcrumb(vm.path);
        check(crumbs.size() == 1 && crumbs[0].text == title,
              "archive breadcrumb keeps the filename and member prefix as its label");
        check(crumbs.size() == 1 && crumbs[0].path == address &&
              fs::NormalizePath(crumbs[0].path) == address,
              "archive click preserves the virtual address without CWD resolution");
        check(vm.header_text == title, "archive pane header retains its title");
    };
    archive_case(L"pulse:archive:\\\\?\\E:\\Downloads\\ATRI.zip|", L"ATRI.zip");
    archive_case(L"pulse:archive:\\\\?\\E:\\Downloads\\ATRI.zip|assets/images/",
                 L"ATRI.zip / assets/images/");
    archive_case(L"pulse:archive:\\\\?\\UNC\\server\\share\\ATRI.zip|folder/", L"ATRI.zip / folder/");
    archive_case(L"pulse:archive:E:\\Downloads\\ATRI.zip", L"ATRI.zip");
    archive_case(L"pulse:archive:\\\\?\\E:\\Downloads\\" + std::wstring(280, L'x') + L"\\资料.zip|",
                 L"资料.zip");

    app::Pane home;
    home.NewTab(app::MakeHomePath());
    home.ActiveTab()->virtual_title = l10n::Get(l10n::StringId::Home);
    ui::PaneViewModel home_vm;
    app::FillPaneViewModel(home_vm, home, nullptr);
    const auto home_crumbs = ui::SplitBreadcrumb(home_vm.path);
    check(home_vm.path == app::MakeHomePath() && home_vm.is_home,
        "home pane retains its virtual address instead of the localized display title");
    check(home_crumbs.size() == 1 && home_crumbs[0].text == l10n::Get(l10n::StringId::Home) &&
          home_crumbs[0].path == app::MakeHomePath(), "home breadcrumb displays Home and targets pulse:home");
    const auto home_generation = home.ActiveTab()->view_generation;
    home.ActiveTab()->NavigateTo(fs::NormalizePath(home_crumbs[0].path));
    check(home.ActiveTab()->current_path == app::MakeHomePath() &&
          home.ActiveTab()->view_generation == home_generation && home.ActiveTab()->back_stack.empty(),
        "clicking current home breadcrumb never resolves Home against the process working directory");

    for (const auto* locale : {L"zh-CN", L"en-US"}) {
        l10n::Initialize(GetModuleHandleW(nullptr), locale);
        for (const auto& address : {std::wstring{}, app::MakeHomePath(), app::MakeStarredPath(),
                 app::MakeRecentPath(), app::MakeRecyclePath(), app::MakeTagPath(L"tag-identity"),
                 app::MakeSearchPath(L"path:E:\\Documents content:needle"),
                 std::wstring{L"pulse:saved-search:7"}, app::MakeSettingsPath(),
                 app::MakeWorkspacePath(0), std::wstring{L"pulse:changes:workspace-identity"}}) {
            app::Pane pane;
            pane.NewTab(address);
            auto& tab = *pane.ActiveTab();
            tab.virtual_title = address.empty() ? std::wstring{}
                : L"Custom display title / 自定义名称";
            const auto title = tab.virtual_title;
            tab.pending_generation = 42;
            tab.loading = false;
            ui::PaneViewModel vm;
            app::FillPaneViewModel(vm, pane, nullptr);
            check(vm.path == address,
                "every virtual pane retains its address independently of locale and display title");
            const auto crumbs = ui::SplitBreadcrumb(vm.path);
            check(!crumbs.empty() && crumbs.back().path == address,
                "every virtual pane breadcrumb targets its own address, including empty This PC");
            const auto titled = ui::SplitBreadcrumb(vm.path, vm.header_text);
            check(!titled.empty() && (address.empty() || titled.back().text == title) &&
                  titled.back().path == address,
                "virtual breadcrumb labels retain custom titles without changing their targets");
            const auto generation = tab.view_generation;
            if (!crumbs.empty()) {
                for (int repeat = 0; repeat < 3; ++repeat)
                    tab.NavigateTo(fs::NormalizePath(crumbs.back().path));
            }
            check(tab.current_path == address && tab.view_generation == generation &&
                  tab.back_stack.empty() && tab.pending_generation == 42 && tab.virtual_title == title,
                "repeated virtual pane clicks preserve location, history and pending work");
        }
    }
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");

    for (const auto& address : {fs::NormalizePath(L"E:\\fixture\\此电脑"),
             fs::NormalizePath(L"\\\\server\\share\\Home")}) {
        app::Pane pane;
        pane.NewTab(address);
        pane.ActiveTab()->virtual_title = L"Display override";
        ui::PaneViewModel vm;
        app::FillPaneViewModel(vm, pane, nullptr);
        const auto crumbs = ui::SplitBreadcrumb(vm.path, vm.header_text);
        check(!crumbs.empty() && fs::NormalizePath(crumbs.back().path) == address &&
              crumbs.back().text != vm.header_text,
            "filesystem addresses, including localized-looking names, remain independent of display overrides");
    }

    app::Tab pc_history;
    pc_history.scroll_y = 17.0f;
    pc_history.NavigateTo(app::MakeHomePath());
    check(pc_history.CanGoBack() && pc_history.GoBack().empty() && pc_history.scroll_y == 17.0f,
        "leaving This PC retains its empty destination and viewport in Back history");
    check(pc_history.CanGoForward() && pc_history.GoForward() == app::MakeHomePath(),
        "Forward restores the destination after returning to This PC");

    const auto local = ui::SplitBreadcrumb(L"\\\\?\\E:\\Downloads\\folder");
    check(local.size() == 4 && local[0].path.empty() && local[1].path == L"E:\\" &&
          local[3].path == L"E:\\Downloads\\folder", "local breadcrumbs keep absolute ancestor paths");
    const auto unc = ui::SplitBreadcrumb(L"\\\\?\\UNC\\server\\share\\folder");
    check(unc.size() == 3 && unc[0].path == L"\\\\server" &&
          unc[2].path == L"\\\\server\\share\\folder", "UNC breadcrumbs keep their network root");
    const auto search = ui::SplitBreadcrumb(L"pulse:search:path:E:\\Downloads content:test");
    check(search.size() == 1 && search[0].path == L"pulse:search:path:E:\\Downloads content:test",
          "search breadcrumbs preserve their virtual address");
    const auto recycle = ui::SplitBreadcrumb(L"pulse:recycle");
    check(recycle.size() == 2 && recycle[0].path.empty() && recycle[1].path == L"pulse:recycle",
          "recycle breadcrumbs retain This PC and Recycle Bin");
    const auto pc = ui::SplitBreadcrumb(L"");
    check(pc.size() == 1 && pc[0].path.empty(), "This PC breadcrumb retains its empty click target");

    const auto repeat_current = [&](const std::wstring& address) {
        app::Tab tab;
        tab.current_path = L"pulse:home";
        tab.NavigateTo(address);
        tab.loading = false;
        tab.selected_index = 1;
        tab.selected.insert(1);
        tab.scroll_y = 123.0f;
        tab.scroll_x = 45.0f;
        tab.filter_text = L"keep";
        tab.virtual_title = L"Current location";
        tab.SetBannerTitle(app::BannerType::Other, L"keep banner");
        tab.pending_generation = 42;
        const auto generation = tab.view_generation;
        const auto back_count = tab.back_stack.size();
        const auto crumbs = ui::SplitBreadcrumb(tab.current_path);
        for (int i = 0; i < 5; ++i) tab.NavigateTo(fs::NormalizePath(crumbs.back().path));
        check(tab.back_stack.size() == back_count,
            "repeated current breadcrumb does not add back history or origin metadata");
        check(tab.view_generation == generation && tab.pending_generation == 42 && !tab.loading,
            "repeated current breadcrumb does not restart a view or its pending load");
        check(tab.selected_index == 1 && tab.selected.contains(1) && tab.scroll_y == 123.0f &&
              tab.scroll_x == 45.0f && tab.filter_text == L"keep" &&
              tab.virtual_title == L"Current location" && tab.banner_title == L"keep banner",
            "repeated current breadcrumb preserves selection, scrolling and view state");
        check(tab.GoBack() == L"pulse:home" && tab.back_stack.empty(),
            "one Back returns to the previous distinct location after repeated clicks");
        const auto forward_count = tab.forward_stack.size();
        tab.NavigateTo(L"pulse:home");
        check(tab.forward_stack.size() == forward_count,
            "reopening current location after Back preserves forward history");
        check(tab.GoForward() == address, "Forward still returns to the original location");
    };
    repeat_current(fs::NormalizePath(L"E:\\Projects"));
    repeat_current(fs::NormalizePath(L"\\\\server\\share\\Projects"));
    repeat_current(L"pulse:archive:\\\\?\\E:\\Projects\\资料.zip|");
    repeat_current(L"pulse:archive:\\\\?\\E:\\Projects\\资料.zip|assets/images/");
    repeat_current(L"");

    app::Tab aliases;
    aliases.current_path = fs::NormalizePath(L"E:\\Projects");
    const auto alias_generation = aliases.view_generation;
    aliases.NavigateTo(L"e:/Projects/");
    aliases.NavigateTo(L"\\\\?\\E:\\PROJECTS\\");
    check(aliases.back_stack.empty() && aliases.view_generation == alias_generation,
        "filesystem case, separators and extended-path spelling do not create duplicate history");

    app::Tab distinct;
    const std::wstring first = L"pulse:archive:\\\\?\\E:\\Projects\\资料.zip|Images/";
    const std::wstring second = L"pulse:archive:\\\\?\\E:\\Projects\\资料.zip|images/";
    distinct.current_path = first;
    distinct.NavigateTo(second);
    distinct.NavigateTo(first);
    check(distinct.back_stack.size() == 2 && distinct.GoBack() == second && distinct.GoBack() == first,
        "distinct archive member case and nonconsecutive revisits remain in navigation history");

    app::Tab search_history;
    search_history.current_path = fs::NormalizePath(L"E:\\Projects");
    const auto search_path = std::wstring(L"pulse:search:files");
    search_history.NavigateTo(search_path);
    const auto origin = search_history.search_origin_path;
    search_history.NavigateTo(search_path);
    check(search_history.back_stack.size() == 1 && search_history.search_origin_valid &&
          search_history.search_origin_path == origin,
        "repeated virtual navigation preserves its search origin without duplicating history");

    app::Tab mixed_history;
    const auto first_origin = fs::NormalizePath(L"E:\\First");
    const auto second_origin = fs::NormalizePath(L"\\\\server\\share\\Second");
    mixed_history.current_path = first_origin;
    mixed_history.scroll_x = 13.0f;
    mixed_history.scroll_y = 131.0f;
    mixed_history.NavigateTo(search_path);
    mixed_history.scroll_x = 25.0f;
    mixed_history.scroll_y = 251.0f;
    mixed_history.NavigateTo(second_origin);
    mixed_history.back_stack.push({L"pulse:search:legacy"});
    mixed_history.GoBack();
    check(!mixed_history.search_origin_valid && mixed_history.search_origin_path.empty() &&
          !mixed_history.pending_history_viewport && mixed_history.scroll_x == 0.0f &&
          mixed_history.scroll_y == 0.0f,
        "path-only search history has no inherited origin or viewport");
    mixed_history.GoBack();
    check(mixed_history.current_path == search_path && mixed_history.search_origin_valid &&
          mixed_history.search_origin_path == first_origin,
        "a path-only history entry does not erase the earlier search origin");
    check(mixed_history.pending_history_viewport && mixed_history.scroll_x == 25.0f &&
          mixed_history.scroll_y == 251.0f,
        "a path-only history entry does not erase the earlier viewport");
    mixed_history.scroll_x = 35.0f;
    mixed_history.scroll_y = 351.0f;
    mixed_history.GoForward();
    mixed_history.GoBack();
    check(mixed_history.search_origin_valid && mixed_history.search_origin_path == first_origin &&
          mixed_history.scroll_x == 35.0f && mixed_history.scroll_y == 351.0f,
        "back and forward save the latest viewport with its search origin");
    mixed_history.NavigateTo(second_origin);
    check(!mixed_history.CanGoForward() && !mixed_history.pending_history_viewport &&
          !mixed_history.search_origin_valid,
        "new directory navigation clears forward history, pending viewport and search origin");
    mixed_history.scroll_x = 45.0f;
    mixed_history.scroll_y = 451.0f;
    mixed_history.NavigateTo(search_path);
    mixed_history.scroll_x = 55.0f;
    mixed_history.scroll_y = 551.0f;
    mixed_history.NavigateTo(L"pulse:home");
    mixed_history.GoBack();
    check(mixed_history.search_origin_valid && mixed_history.search_origin_path == second_origin &&
          mixed_history.scroll_x == 55.0f && mixed_history.scroll_y == 551.0f,
        "the same query opened from another folder restores its own origin and viewport");
    mixed_history.GoBack();
    check(mixed_history.current_path == second_origin && !mixed_history.search_origin_valid &&
          mixed_history.scroll_x == 45.0f && mixed_history.scroll_y == 451.0f,
        "returning from search restores the UNC folder viewport and clears the origin");
    mixed_history.GoBack();
    check(mixed_history.current_path == search_path && mixed_history.search_origin_valid &&
          mixed_history.search_origin_path == first_origin && mixed_history.scroll_x == 35.0f &&
          mixed_history.scroll_y == 351.0f,
        "earlier occurrences of the same query retain their original metadata");
    mixed_history.GoForward();
    mixed_history.GoForward();
    check(mixed_history.current_path == search_path && mixed_history.search_origin_valid &&
          mixed_history.search_origin_path == second_origin && mixed_history.scroll_x == 55.0f &&
          mixed_history.scroll_y == 551.0f,
        "forward restores the later occurrence of a repeated query independently");

    app::Tab empty_origin;
    empty_origin.NavigateTo(search_path);
    empty_origin.NavigateTo(L"pulse:home");
    empty_origin.GoBack();
    check(empty_origin.search_origin_valid && empty_origin.search_origin_path.empty(),
        "This PC search origin remains distinct from missing origin metadata");
    empty_origin.back_stack.push({first_origin});
    empty_origin.GoBack();
    check(empty_origin.current_path == first_origin && !empty_origin.search_origin_valid &&
          !empty_origin.pending_history_viewport,
        "path-only folder history remains navigable without optional metadata");
    return failures ? 1 : 0;
}
