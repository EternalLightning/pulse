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
    return failures ? 1 : 0;
}
