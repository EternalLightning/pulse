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
        check(tab.back_stack.size() == back_count && tab.back_search_origins.size() == back_count,
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
        check(tab.forward_stack.size() == forward_count && tab.forward_search_origins.size() == forward_count,
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
    return failures ? 1 : 0;
}
