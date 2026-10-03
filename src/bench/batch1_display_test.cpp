#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "../common/drive_labels.h"
#include "../app/builtin_menu_items.h"
#include "../app/context_menu.h"
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    check(format::LocalFileTime({}).empty() && format::LocalFileTime({}, L"unknown") == L"unknown",
        "missing timestamps never show the FILETIME epoch");
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (const UINT kind : {DRIVE_FIXED, DRIVE_REMOVABLE, DRIVE_REMOTE, DRIVE_CDROM, DRIVE_RAMDISK}) {
            fs::DirEntry drive;
            drive.name = L"Q:"; drive.full_path = L"Q:\\";
            drive.is_dir = true; drive.drive_type = kind;
            ui::PaneViewModel pane;
            pane.snapshot = std::make_shared<std::vector<fs::DirEntry>>(1, drive);
            const auto& row = ui::MakeVisibleEntry(pane, 0);
            check(row.type_text == format::DriveTypeText(kind) && !row.type_text.empty() &&
                row.date_text.empty() && row.name == row.type_text + L" (Q:)",
                "drive type and empty date reach visible rows in the selected language");
        }
    }
    l10n::SetLanguage(L"zh-CN");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true; s.appPrefs.persist = s.places.persist = false;
    const HWND foreground = GetForegroundWindow();
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"", WS_OVERLAPPEDWINDOW,
        -20000, -20000, 1100, 760, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        s.compositor.RecreateTextFormats(scale);
        const auto only_new_tab = ui::LayoutNameTrail(0, 0, 24*scale, 180*scale, 0, 32*scale,
            scale, std::wstring(80, L'x'), 0, 0, true, true, true,
            &s.compositor, s.compositor.DwriteFactory(), s.compositor.FileNameFormat(),
            false, 3, {}, 2u);
        check(only_new_tab.show_new_tab && only_new_tab.new_tab.right > only_new_tab.new_tab.left &&
            !only_new_tab.show_star && !only_new_tab.show_more,
            "a long name cannot collapse the sole enabled row button into a hidden more menu");
    }
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = L"C:\\fixture\\" + std::wstring(180, L'长');
    auto rows = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*rows)[0].name = std::wstring(180, L'长') + L".txt";
    (*rows)[1].name = L"short.txt";
    for (auto& row : *rows) row.full_path = tab.current_path + L"\\" + row.name;
    tab.SetSnapshot(rows); tab.loading = false;
    const auto output = std::filesystem::absolute(argc == 2 ? std::filesystem::path(argv[1]) :
        std::filesystem::path(L"../bench_data/integration-display"));
    std::filesystem::create_directories(output);
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
        s.scale = scale; s.darkMode = dark;
        s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(720 * scale), static_cast<UINT>(760 * scale));
        auto vm = BuildVm(s, false);
        const auto rect = D2D1::RectF(0, 0, 720 * scale, 760 * scale);
        std::vector<ui::MainRenderer::BreadcrumbPlaced> placed;
        s.renderer.BreadcrumbLayout(vm.pane, rect.right, placed);
        const auto address = s.renderer.AddressBarRect(rect.right);
        check(!placed.empty() && placed.back().rc.right <= address.right &&
            placed.back().text.find(L'\u2026') != std::wstring::npos &&
            placed.back().path == tab.current_path,
            "long final breadcrumb is ellipsized inside the bar without changing navigation path");
        const auto& last = placed.back();
        const auto hit = s.renderer.HitTest(vm, rect,
            (last.rc.left + last.rc.right) / 2, (last.rc.top + last.rc.bottom) / 2);
        check(hit.path == tab.current_path && hit.label == last.full_text,
            "truncated breadcrumb exposes the full name on hover");
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm, rect, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "display render completes");
        check(s.renderer.NameWasTruncated((*rows)[0].full_path) &&
            !s.renderer.NameWasTruncated((*rows)[1].full_path),
            "row hover distinguishes cropped long names from fully displayed short names");
        const auto file = output / (std::wstring(dark ? L"dark-" : L"light-") +
            std::to_wstring(static_cast<int>(scale * 100)) + L".png");
        check(s.compositor.SaveSnapshot(file.c_str()), "display screenshot saved");
        s.ctxMenuPrefs.SetBuiltinGroupEnabled(false);
        vm = BuildVm(s, false);
        check(vm.pane.row_action_mask == 0 && std::all_of(vm.pane_slots.begin(), vm.pane_slots.end(),
            [](const auto& slot) { return slot.pane.row_action_mask == 0; }),
            "hidden row buttons reach both focused and slot pane models");
        const auto pane_rect = vm.pane_slots.empty() ? s.renderer.ContentRect(rect.right, rect.bottom)
                                                   : vm.pane_slots.front().rect;
        const auto cell = s.renderer.ItemRectInPane(vm.pane, pane_rect, 1);
        vm.pane.hover_index = 1;
        if (!vm.pane_slots.empty()) vm.pane_slots.front().pane.hover_index = 1;
        int hidden_hits = 0;
        for (int x = static_cast<int>(cell.left + 1); x < static_cast<int>(cell.right); ++x) {
            const auto row_hit = s.renderer.HitTest(vm, rect, static_cast<float>(x), (cell.top + cell.bottom) / 2);
            if (row_hit.region == ui::HitTestResult::RowStar || row_hit.region == ui::HitTestResult::RowNewTab ||
                row_hit.region == ui::HitTestResult::RowMore) ++hidden_hits;
        }
        check(hidden_hits == 0, "hidden file-row actions leave no clickable hit regions");
        s.ctxMenuPrefs.SetBuiltinGroupEnabled(true);
        check(BuildVm(s, false).pane.row_action_mask == 7u, "restoring built-in items restores row buttons");
    }
    auto second_pane = std::make_unique<app::Pane>();
    second_pane->NewTab(tab.current_path);
    second_pane->view.SetSnapshot(rows);
    auto& layout_tab = *s.window_tabs.Active();
    layout_tab.panes.push_back(std::move(second_pane));
    layout_tab.layout = app::LayoutPreset::TwoVertical;
    app::RebuildLayoutRoot(layout_tab);
    s.ctxMenuPrefs.SetBuiltinGroupEnabled(false);
    const auto split_vm = BuildVm(s, false);
    check(split_vm.pane_slots.size() == 2 && std::all_of(split_vm.pane_slots.begin(), split_vm.pane_slots.end(),
        [](const auto& slot) { return slot.pane.row_action_mask == 0; }),
        "row button visibility is applied to focused and non-focused split panes");
    s.ctxMenuPrefs.SetBuiltinGroupEnabled(true);
    layout_tab.layout = app::LayoutPreset::Single;
    app::RebuildLayoutRoot(layout_tab);
    layout_tab.panes.pop_back();
    auto scrollbar_vm = BuildVm(s, false);
    ui::SidebarGroup long_group;
    long_group.id = 9123;
    long_group.header = L"Scrollbar fixture";
    long_group.expansion = 1.0f;
    for (int index = 0; index < 40; ++index) {
        ui::SidebarItem item;
        item.label = L"Folder " + std::to_wstring(index);
        item.path = L"C:\\fixture\\" + std::to_wstring(index);
        long_group.items.push_back(std::move(item));
    }
    scrollbar_vm.sidebar = {long_group};
    s.renderer.SetSidebarWidthDip(224);
    const float sidebar_window_w = 1500 * s.scale;
    const auto sidebar = s.renderer.SidebarRect(sidebar_window_w, 1140);
    scrollbar_vm.sidebar_scrollbar_opacity = 0.0f;
    scrollbar_vm.sidebar_scrollbar_expand = 0.0f;
    auto bar = ui::SidebarScrollbarSpec(scrollbar_vm, sidebar, s.scale);
    auto thin = ui::fluent::ScrollbarThumbRect(bar, s.scale);
    const auto track_hit = s.renderer.HitTest(scrollbar_vm, D2D1::RectF(0, 0, sidebar_window_w, 1140),
        (bar.viewport.left + bar.viewport.right) / 2, (thin.top + thin.bottom) / 2);
    check(track_hit.region == ui::HitTestResult::Scrollbar && track_hit.pane_index < 0,
        "hidden sidebar scrollbar keeps its hover and drag track reachable");
    scrollbar_vm.sidebar_scrollbar_opacity = 1.0f;
    scrollbar_vm.sidebar_scrollbar_expand = 1.0f;
    bar = ui::SidebarScrollbarSpec(scrollbar_vm, sidebar, s.scale);
    const auto expanded = ui::fluent::ScrollbarThumbRect(bar, s.scale);
    check(expanded.right - expanded.left > thin.right - thin.left,
        "hovered scrollbar geometry expands without losing its drag target");
    const auto contrast = ui::fluent::ScrollbarThumbRect(bar, s.scale, true);
    check(contrast.right - contrast.left >= expanded.right - expanded.left,
        "high contrast keeps the scrollbar at its accessible full width");
    const auto saved_path = tab.current_path;
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (int page : {0, 2}) {
            tab.current_path = app::MakeSettingsPath(page == 0 ? L"general" : L"context");
            s.settings.SelectPage(page);
            s.settingsExpanded = ui::kSettingsDefaultExpandedMask;
            auto settings_vm = BuildVm(s, false);
            ui::fluent::Painter layout_painter;
            layout_painter.SetCompositor(&s.compositor);
            layout_painter.SetScale(s.scale);
            layout_painter.BeginFrame(ui::MakeTheme(true, ui::HexColor(0x0078D4)), false);
            const auto rect = D2D1::RectF(0, 0, 1080, 1140);
            const auto layout = ui::MakeSettingsLayout(settings_vm, rect, s.scale,
                s.renderer.TitleBarHeight(), 28 * s.scale, &layout_painter);
            if (page == 0) {
                check(layout.startup_tray_row.bottom <= layout.startup_row[1].top &&
                    layout.last_tab_row.bottom <= layout.startup_row[2].top,
                    "new startup and last-tab option rows do not overlap");
                check(layout.startup_tray_row.right > layout.startup_tray_row.left &&
                    layout.default_manager_rows[3].bottom > layout.default_manager_rows[3].top,
                    "startup and experimental manager settings remain reachable in narrow layout");
            } else {
                check(settings_vm.settings_items.size() >= app::kBuiltinMenuItemCount &&
                    layout.context_cards[5].bottom > layout.context_cards[5].top,
                    "settings include a dedicated Pulse built-in commands group");
                const auto description = ui::SettingsToggleBounds(layout.context_header[5],
                    l10n::Get(app::builtin_text::GroupDescription), s.scale, &layout_painter, 112).description;
                check(description.bottom <= layout.context_header[5].bottom &&
                    description.right <= layout.context_cards[5].right,
                    "built-in settings description fits its localized card");
            }
            s.settings.SetScroll(page == 0 ? layout.startup_tray_row.top - layout.content_origin :
                layout.context_header[5].top - layout.content_origin, 10000);
            settings_vm = BuildVm(s, false);
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(settings_vm, rect, ui::MakeTheme(true, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "new settings section renders without drawing errors");
            const auto settings_image = output / (std::wstring(page == 0 ? L"system-settings-" : L"builtin-settings-") +
                language + L".png");
            check(s.compositor.SaveSnapshot(settings_image.c_str()), "new settings screenshot saved");
        }
    }
    tab.current_path = saved_path;
    s.settings.SetScroll(0, 10000);
    app::ContextMenuPrefs menu_prefs;
    menu_prefs.persist = false;
    menu_prefs.SetBuiltinItemEnabled(app::BuiltinMenuItem::Tags, false);
    menu_prefs.SetBuiltinItemEnabled(app::BuiltinMenuItem::SelectCommands, false);
    menu_prefs.SetBuiltinItemEnabled(app::BuiltinMenuItem::RowStar, false);
    app::ContextMenuPrefs loaded_menu_prefs;
    loaded_menu_prefs.persist = false;
    check(loaded_menu_prefs.FromJson(menu_prefs.ToJson()) &&
        !loaded_menu_prefs.BuiltinItemEnabled(app::BuiltinMenuItem::Tags) &&
        !loaded_menu_prefs.BuiltinItemEnabled(app::BuiltinMenuItem::SelectCommands) &&
        app::RowActionMask(loaded_menu_prefs.builtin_hidden) == 6u,
        "built-in menu and button visibility round trips independently");
    auto menu_items = app::FilterBuiltinMenuItems(app::BuildBackgroundMenu(false, true, L"Undo"), menu_prefs.builtin_hidden);
    const auto has_command = [&](int command) {
        for (const auto& item : menu_items) {
            if (item.command == command) return true;
            for (const auto& child : item.children) if (child.command == command) return true;
        }
        return false;
    };
    check(!has_command(app::CmdSelectAll) && !has_command(app::CmdInvertSelection),
        "hidden selection commands are absent from the rendered menu");
    check(loaded_menu_prefs.FromJson(L"{}") && loaded_menu_prefs.builtin_hidden == 0,
        "legacy context preferences keep Pulse commands and row buttons visible");
    auto measure = [](const std::wstring& text) { return static_cast<float>(text.size()); };
    const auto compact = ui::MiddleEllipsisPath(L"C:\\folder\\" + std::wstring(80, L'x'), 18, measure);
    check(measure(compact.first + compact.second) <= 18 &&
        compact.second.find(L'\u2026') != std::wstring::npos, "long path leaf also fits after middle ellipsis");
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    check(GetForegroundWindow() == foreground, "display fixture preserves foreground activation");
    OleUninitialize();
    return failures ? 1 : 0;
}
