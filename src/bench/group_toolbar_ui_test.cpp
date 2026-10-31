// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/toolbar_layout.h"
#include "../common/resource_ids.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

int RunGroupToolbarUiTest(bool order_only = false) {
    using namespace pulse;
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto output = std::filesystem::absolute(order_only
        ? L"docs/validation/group-order-20261030" : L"bench_data/group-toolbar-ui");
    std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.accentColor = ui::HexColor(0x0078D4);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Group toolbar fixture",
        WS_POPUP, -30000, -30000, 1100, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 1;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(false);
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.window_tabs.NewTab(L"C:\\fixture");
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    if (order_only) {
        SetGroup(s, ui::GroupMode::Type, ui::SortDirection::Desc);
        DispatchMenuCommand(s, app::CmdGroupAscending);
        check(tab->group_mode == ui::GroupMode::Type && tab->group_direction == ui::SortDirection::Asc,
            "ascending command sets group order explicitly");
        DispatchMenuCommand(s, app::CmdGroupAscending);
        check(tab->group_direction == ui::SortDirection::Asc, "selecting ascending twice does not toggle back");
        DispatchMenuCommand(s, app::CmdGroupDescending);
        check(tab->group_direction == ui::SortDirection::Desc, "descending command sets group order explicitly");
        SetGroup(s, ui::GroupMode::None, ui::SortDirection::Desc);
        DispatchMenuCommand(s, app::CmdGroupAscending);
        check(tab->group_mode == ui::GroupMode::None && tab->group_direction == ui::SortDirection::Desc,
            "direction command does not change an ungrouped view");
        for (const auto* language : {L"zh-CN", L"en-US"}) for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            l10n::Initialize(GetModuleHandleW(nullptr), language);
            s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
            ui::FluentMenu menu;
            check(menu.Create(s.hwnd, &s.compositor, scale), "create grouping direction menu");
            menu.SetTheme(dark, s.accentColor);
            const auto name = std::wstring(L"group-order-") + language + (dark ? L"-dark-" : L"-light-") +
                std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            check(menu.SaveDebugSnapshot((output / name).c_str(),
                app::BuildGroupingMenu(ui::GroupMode::Type, ui::SortDirection::Desc, true)),
                "render explicit ascending and descending menu choices");
        }
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    check(FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_FILES_GROUP_SVG), RT_RCDATA) != nullptr,
        "user-provided grouping SVG is embedded in the production resource set");
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::Initialize(GetModuleHandleW(nullptr), language);
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            s.scale = scale;
            s.darkMode = dark;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            for (int width : {1100, 960, 700, 380}) for (float expand : {0.0f, 1.0f}) {
                s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(720 * scale));
                s.pane->filter_expand = expand;
                auto vm = BuildVm(s, false);
                check(vm.can_group, "ordinary folder enables independent grouping");
                const auto rect = D2D1::RectF(0, 0, width * scale, 720 * scale);
                const float left = s.renderer.EffectiveSidebarWidth(rect.right);
                const auto layout = ui::MakeToolbarLayout(rect.right, scale, s.renderer.TitleBarHeight(),
                    s.renderer.Margin(), s.renderer.NewButtonWidthPx(rect.right - left < 600 * scale),
                    left, expand);
                if (layout.group.right - layout.group.left >= 60 * scale) {
                    ui::fluent::Painter painter(&s.compositor);
                    painter.SetScale(scale);
                    painter.BeginFrame(ui::MakeTheme(dark, s.accentColor));
                    check(painter.MeasureButtonWidth(l10n::Get(l10n::StringId::ToolbarGroup), L"\xE8FD", true) <=
                        layout.group.right - layout.group.left,
                        "localized group label, supplied icon and chevron fit without clipping");
                }
                check(layout.sort.right + 5 * scale <= layout.group.left &&
                    layout.group.right + 5 * scale <= layout.filter.left &&
                    layout.filter.right + 7 * scale <= layout.overflow.left,
                    "sort, group, filter and overflow remain separated while filter expands");
                check(layout.sort.left >= layout.create.right && layout.group.right <= rect.right,
                    "compact toolbar stays inside available space");
                if (layout.commands[5].right > layout.commands[5].left)
                    check(layout.commands[5].right + 7 * scale <= layout.sort.left,
                        "file actions remain separated from the three independent controls");
                for (int context : {1, 2}) {
                    const auto contextual = ui::MakeToolbarLayout(rect.right, scale, s.renderer.TitleBarHeight(),
                        s.renderer.Margin(), s.renderer.NewButtonWidthPx(rect.right - left < 600 * scale),
                        left, expand, context == 1, context == 2);
                    const auto action = context == 1 ? contextual.extract_all : contextual.empty_recycle;
                    check(action.right <= action.left || action.right + 7 * scale <= contextual.sort.left,
                        "archive and recycle actions fit before the new grouping control");
                }
                const auto hit = [&](D2D1_RECT_F bounds) {
                    return s.renderer.HitTest(vm, rect, (bounds.left + bounds.right) * 0.5f,
                        (bounds.top + bounds.bottom) * 0.5f).region;
                };
                check(hit(layout.group) == ui::HitTestResult::ToolbarGroup &&
                    hit(layout.sort) == ui::HitTestResult::ToolbarSort &&
                    hit(layout.filter) == ui::HitTestResult::FilterBox,
                    "production hit testing routes independent group, sort and filter buttons");
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(vm, rect, ui::MakeTheme(dark, s.accentColor));
                check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "production toolbar renders successfully");
                if (expand == 0.0f && (width == 1100 || width == 380)) {
                    const auto name = std::wstring(language) + L"-" + std::to_wstring(width) + L"-" +
                        std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png");
                    check(s.compositor.SaveSnapshot((output / name).c_str()), "save production toolbar frame");
                }
            }
            ui::FluentMenu menu;
            check(menu.Create(s.hwnd, &s.compositor, scale), "create grouping popup renderer");
            menu.SetTheme(dark, s.accentColor);
            for (const auto mode : {ui::GroupMode::Auto, ui::GroupMode::None, ui::GroupMode::DateModified,
                                    ui::GroupMode::Type, ui::GroupMode::Size}) {
                const auto items = app::BuildGroupingMenu(mode, ui::SortDirection::Desc, true);
                check(items.size() == 7 && std::all_of(items.begin(), items.end(),
                    [](const auto& item) { return item.children.empty(); }),
                    "grouping choices are flat with no wrapping submenu");
                check(std::count_if(items.begin(), items.begin() + 5, [](const auto& item) { return item.radio; }) == 1 &&
                    !items[5].radio && items[6].radio,
                    "group menu retains one selected mode and one selected direction");
                check(items.back().enabled == (mode != ui::GroupMode::Auto && mode != ui::GroupMode::None),
                    "group order follows existing mode restrictions");
            }
            const auto name = std::wstring(language) + L"-menu-" + std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png");
            check(menu.SaveDebugSnapshot((output / name).c_str(),
                app::BuildGroupingMenu(ui::GroupMode::Type, ui::SortDirection::Desc, true)),
                "render the direct grouping menu without displaying a desktop popup");
        }
    }
    for (const auto* path : {L"", L"pulse://home", L"pulse://recycle", L"pulse://search"}) {
        tab->current_path = path;
        const auto vm = BuildVm(s, false);
        check(!vm.can_group, "unsupported virtual or empty location disables grouping");
        const auto rect = D2D1::RectF(0, 0, 1100 * s.scale, 720 * s.scale);
        const float left = s.renderer.EffectiveSidebarWidth(rect.right);
        const auto layout = ui::MakeToolbarLayout(rect.right, s.scale, s.renderer.TitleBarHeight(),
            s.renderer.Margin(), s.renderer.NewButtonWidthPx(rect.right - left < 600 * s.scale),
            left, s.pane->filter_expand);
        check(s.renderer.HitTest(vm, rect, (layout.group.left + layout.group.right) * 0.5f,
            (layout.group.top + layout.group.bottom) * 0.5f).region != ui::HitTestResult::ToolbarGroup,
            "disabled grouping has no active hit target");
    }
    const auto sort = app::BuildSortMenu({});
    check(std::none_of(sort.begin(), sort.end(), [](const auto& item) {
        return (item.command >= app::CmdGroupAuto && item.command <= app::CmdGroupSize) ||
            item.command == app::CmdGroupAscending || item.command == app::CmdGroupDescending;
    }), "sorting menu contains no grouping commands");
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const int result = RunGroupToolbarUiTest(argc == 2 && std::string_view(argv[1]) == "--group-order-only");
    OleUninitialize();
    return result;
}
