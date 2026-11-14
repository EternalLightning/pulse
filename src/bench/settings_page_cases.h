// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
#pragma once

#include "../ui/toolbar_layout.h"
#include "../ui/home_layout.h"

int RunSettingsPageCases(bool transfer_only = false, bool appearance_only = false) {
    using namespace pulse;
    using H = ui::HitTestResult;
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cout << "[FAIL] " << label << '\n'; }
    };
    const auto output = std::filesystem::absolute(L".tmp/settings-page");
    std::filesystem::create_directories(output);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Settings page fixture", WS_POPUP, -20000, -20000, 1280, 900,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ui::Compositor compositor;
    ui::MainRenderer renderer;
    if (!window || !compositor.Init(window)) return 2;
    renderer.SetCompositor(&compositor);
    renderer.SetDetailsPanelVisible(false);
    ui::fluent::Painter painter(&compositor);
    ui::WindowViewModel vm;
    vm.settings_open = true;
    ui::TabView downloads;
    downloads.title = L"Downloads";
    ui::TabView settings;
    settings.title = L"Settings";
    settings.is_settings = settings.active = true;
    vm.tabs = {downloads, settings};
    vm.pane.path = L"pulse:settings/tabs";
    vm.settings_about_rows = {{L"Version", L"1.4.1.0"}, {L"Build", L"fixture"},
        {L"System", L"Windows 11"}, {L"Display", L"Direct2D"}};
    ui::SidebarGroup group;
    group.header = L"Pinned";
    ui::SidebarItem item;
    item.label = L"Downloads";
    item.path = L"C:\\Downloads";
    group.items.push_back(item);
    vm.sidebar.push_back(group);
    for (const auto* language : {L"zh-CN", L"en-US"}) for (float scale : {1.0f, 1.5f})
    for (float width : {620.0f, 1100.0f, 1600.0f}) for (float sidebar : {224.0f, 300.0f}) {
        l10n::SetLanguage(language);
        renderer.SetScale(scale);
        renderer.SetSidebarWidthDip(sidebar);
        compositor.RecreateTextFormats(scale);
        compositor.Resize(static_cast<UINT>(width*scale), static_cast<UINT>(900*scale));
        painter.SetScale(scale);
        const auto rect = D2D1::RectF(0, 0, width*scale, 900*scale);
        const float outer = renderer.EffectiveSidebarWidth(rect.right);
        const float title = renderer.TitleBarHeight();
        const auto toolbar = ui::MakeToolbarLayout(rect.right, scale, title, 4*scale, 100*scale, outer);
        auto hit = [&](const D2D1_RECT_F& r) {
            return renderer.HitTest(vm, rect, (r.left+r.right)/2, (r.top+r.bottom)/2);
        };
        for (int page : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
            if (transfer_only && page != 8) continue;
            if (appearance_only && page != 0) continue;
            vm.settings_transfer_status = l10n::Get(l10n::StringId::SettingsTransferDesc);
            vm.settings_page = page;
            vm.settings_scroll = 0;
            const auto layout = ui::MakeSettingsLayout(vm, rect, scale, title, 28*scale, &painter, outer);
            check(layout.body.left > outer && layout.body.top > toolbar.address.bottom,
                "settings occupies the main content area below address navigation");
            const auto address_hit = hit(toolbar.address).region;
            check(hit(toolbar.navigation[0]).region == H::NavBack &&
                (address_hit == H::AddressBar || address_hit == H::BreadcrumbSegment),
                "address navigation retains production hit targets");
            const auto home = ui::HomeButtonRect(outer, title+4*scale, scale);
            check(hit(home).region == H::HomeButton &&
                hit(renderer.SidebarSettingsRect(rect.right, rect.bottom)).region == H::SettingsButton,
                "outer navigation remains usable from settings");
            check(hit(toolbar.create).region != H::NewButton &&
                hit(toolbar.sort).region != H::ToolbarSort,
                "file command bar has no live controls on settings");
            float previous = 0;
            for (const auto& row : layout.category_rows) {
                check(row.top > previous && row.right <= layout.content.right && row.left >= layout.content.left,
                    "individual setting cards have separate backgrounds and remain inside content");
                previous = row.bottom;
            }
            const std::pair<D2D1_RECT_F, D2D1_RECT_F> inline_controls[] = {
                {layout.language_choice, layout.language_card},
                {layout.external_open_choice, layout.external_open_card},
                {layout.wallpaper_fit, layout.wallpaper_fit_card},
                {layout.wallpaper_look_value, layout.wallpaper_look_card},
                {layout.wallpaper_blur_row[0], layout.wallpaper_blur_card},
                {layout.text_render_row[0], layout.text_render_card},
                {layout.folder_sort_row[0], layout.folder_sort_card},
                {layout.density_row[0], layout.density_card},
                {ui::SettingsToggleBounds(layout.startup_tray_row,
                    l10n::Get(l10n::StringId::StartToTrayDesc), scale, &painter).control, layout.startup_tray_row},
                {layout.change_days[0], layout.change_days_row},
                {layout.theme_tile[0], layout.theme_row},
                {layout.accent_picker, layout.accent_card},
                {layout.wallpaper_choose, layout.wallpaper_card},
                {layout.wallpaper_clear, layout.wallpaper_card},
                {layout.global_search_hotkey_button, layout.global_search_hotkey_row},
                {layout.dup_browse, layout.dup_target_row},
                {layout.dup_min_size[0], layout.dup_min_row},
                {layout.dup_scope[0], layout.dup_scope_row},
            };
            for (const auto& [control, row] : inline_controls) {
                if (control.bottom <= control.top || row.right-row.left < 560*scale ||
                    control.left < row.left+100*scale) continue;
                check(std::abs((control.top+control.bottom)-(row.top+row.bottom)) < 0.5f,
                    "inline controls are vertically centered in their setting row");
            }
            if (page == 8) {
                check(layout.transfer_card.top > layout.configuration_path.bottom &&
                    layout.transfer_description.bottom < layout.transfer_action[0].top,
                    "settings transfer follows configuration and keeps its description above actions");
                for (auto id : {l10n::StringId::ExportSettingsTitle, l10n::StringId::ImportSettingsTitle}) {
                    auto origin = vm;
                    origin.settings_scroll = 0;
                    check(std::abs(renderer.SettingsDestinationOffset(origin, static_cast<int>(id),
                        rect.right, rect.bottom) - std::max(0.0f, layout.transfer_card.top -
                        layout.content_origin - 20*scale)) < 0.5f,
                        "settings search scrolls import and export into view");
                }
                for (int state = 0; state < 4; ++state) {
                    vm.settings_transfer_busy = state >= 2;
                    vm.settings_transfer_pending = state % 2 != 0;
                    vm.settings_transfer_status = l10n::Get(vm.settings_transfer_busy ? l10n::StringId::SettingsTransferring :
                        vm.settings_transfer_pending ? l10n::StringId::SettingsTransferPending : l10n::StringId::SettingsTransferDesc);
                    vm.settings_scroll = 0;
                    const auto state_layout = ui::MakeSettingsLayout(vm, rect, scale, title, 28*scale, &painter, outer);
                    check(vm.settings_transfer_pending || state_layout.transfer_action[2].bottom == 0,
                        "restart is absent until an import is pending");
                    for (int i = 0; i < 2 + static_cast<int>(vm.settings_transfer_pending); ++i) {
                        vm.settings_scroll = std::max(0.0f,
                            state_layout.transfer_action[i].top-state_layout.content.top-20*scale);
                        const auto visible = ui::MakeSettingsLayout(vm, rect, scale, title, 28*scale, &painter, outer);
                        const auto bounds = visible.transfer_action[i];
                        check(bounds.left >= visible.transfer_card.left+16*scale &&
                            bounds.right <= visible.transfer_card.right-16*scale+0.5f &&
                            bounds.bottom < visible.transfer_card.bottom,
                            "settings transfer actions fit narrow and high DPI cards");
                        const auto action = hit(bounds);
                        const bool enabled = !vm.settings_transfer_busy;
                        check(enabled ? action.region == H::SettingsDiagnosticsAction && action.index == i+3
                                      : action.region != H::SettingsDiagnosticsAction,
                            "transfer actions honor busy and restart states after scrolling");
                    }
                }
                vm.settings_transfer_busy = vm.settings_transfer_pending = false;
                vm.settings_transfer_status = l10n::Get(l10n::StringId::SettingsTransferDesc);
            }
            if (page == 3) {
                const auto aligned = [&](const auto& buttons, int count, const auto& card) {
                    for (int i = 0; i < count; ++i) {
                        check(buttons[i].left >= card.left+16*scale &&
                            buttons[i].right <= card.right-16*scale+0.5f && buttons[i].bottom < card.bottom,
                            "about action buttons stay inside their card");
                        if (i+1 == count || buttons[i+1].top > buttons[i].top)
                            check(std::abs(buttons[i].right-(card.right-16*scale)) < 0.5f,
                                "each about action row aligns to the right");
                    }
                };
                aligned(layout.about_action, 2, layout.about_card);
                aligned(layout.about_action+2, 1, layout.about_distribution_card);
                aligned(layout.diagnostics_action, 3, layout.diagnostics_card);
                for (int i = 0; i < 3; ++i) {
                    vm.settings_scroll = std::max(0.0f, layout.about_action[i].top-layout.content.top-20*scale);
                    const auto visible = ui::MakeSettingsLayout(vm, rect, scale, title, 28*scale, &painter, outer);
                    const auto action = hit(visible.about_action[i]);
                    check(action.region == H::SettingsAboutAction && action.index == i,
                        "right aligned about actions keep their hit targets after scrolling");
                }
                vm.settings_scroll = renderer.SettingsMaxScroll(vm, rect.right, rect.bottom);
                D2D1_RECT_F track{}, thumb{};
                float maximum = 0;
                if (renderer.SettingsScrollbarGeometry(vm, rect.right, rect.bottom, track, thumb, maximum))
                    check(hit(thumb).region == H::Scrollbar && maximum >= vm.settings_scroll,
                        "settings scrollbar uses the embedded content viewport");
            }
            if (sidebar == 224) for (bool dark : {false, true}) {
                vm.settings_scroll = page == 8 ? std::min(renderer.SettingsMaxScroll(vm, rect.right, rect.bottom),
                    std::max(0.0f, layout.transfer_card.top-layout.content.top-40*scale)) : 0;
                if (appearance_only) {
                    vm.settings_scroll = std::max(0.0f, layout.effect_card.top-layout.content.top-16*scale);
                    const auto visible = ui::MakeSettingsLayout(vm, rect, scale, title, 28*scale, &painter, outer);
                    check(visible.effect_card.top >= visible.content.top &&
                        visible.effect_card.bottom <= visible.content.bottom,
                        "the complete material illustration card is visible in its screenshot viewport");
                    for (int i = 0; i < ui::kWindowEffectCount; ++i) {
                        const auto effect_hit = hit(visible.effect_row[i]);
                        check(effect_hit.region == H::SettingsEffect && effect_hit.index == i,
                            "visible material illustrations retain their four effect hit targets");
                    }
                }
                vm.dark = dark;
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                auto* dc = compositor.Dc();
                ui::ComPtr<IDWriteRenderingParams> before, after;
                dc->GetTextRenderingParams(&before);
                const auto aa = dc->GetTextAntialiasMode();
                dc->BeginDraw();
                renderer.Render(vm, rect, theme);
                check(SUCCEEDED(dc->EndDraw()), "embedded settings transition frame renders");
                Sleep(260);
                dc->BeginDraw();
                renderer.Render(vm, rect, theme);
                check(SUCCEEDED(dc->EndDraw()), "embedded settings renders with native heading antialiasing");
                dc->GetTextRenderingParams(&after);
                check(before.get() == after.get() && aa == dc->GetTextAntialiasMode(),
                    "smooth heading restores text rendering state for other controls");
                const auto filename = std::wstring(language)+L"-"+std::to_wstring(page)+L"-"+
                    std::to_wstring(static_cast<int>(width))+L"-"+std::to_wstring(static_cast<int>(scale*100))+
                    (dark ? L"-dark.png" : L"-light.png");
                if (appearance_only || page == 3 || page == 7 || page == 8 || (width == 1100 && scale == 1))
                    check(bench::SaveSnapshot(compositor, output/filename), "capture embedded settings for visual review");
            }
        }
    }
    if (!transfer_only && !appearance_only) {
        renderer.SetScale(1);
        renderer.SetSidebarWidthDip(224);
        compositor.RecreateTextFormats(1);
        compositor.Resize(1600, 900);
        auto* dc = compositor.Dc();
        dc->BeginDraw();
        dc->Clear(ui::HexColor(0x285377));
        ui::ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(ui::HexColor(0xD7C6A5), &brush);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(1150, 450), 700, 500), brush.get());
        brush->SetColor(ui::HexColor(0x397987));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(300, 800), 900, 300), brush.get());
        check(SUCCEEDED(dc->EndDraw()) && compositor.SaveSnapshot((output/L"wallpaper.png").c_str()),
            "create isolated bright and dark wallpaper fixture");
        vm.window_effect = ui::WindowEffect::None;
        vm.background_image = (output/L"wallpaper.png").wstring();
        vm.wallpaper_visibility = 100;
        vm.wallpaper_blur = 0;
        vm.backdrop_enabled = true;
        vm.settings_scroll = 0;
        l10n::SetLanguage(L"zh-CN");
        for (int page : {3, 7}) for (bool dark : {false, true}) {
            vm.settings_page = page;
            vm.dark = dark;
            dc->BeginDraw();
            renderer.Render(vm, D2D1::RectF(0, 0, 1600, 900), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(dc->EndDraw()), "wallpaper settings transition frame renders");
            Sleep(260);
            dc->BeginDraw();
            renderer.Render(vm, D2D1::RectF(0, 0, 1600, 900), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(dc->EndDraw()), "settings cards and heading render over wallpaper");
            check(bench::SaveSnapshot(compositor, output/(L"wallpaper-"+std::to_wstring(page)+
                (dark ? L"-dark.png" : L"-light.png"))), "capture wallpaper settings for visual review");
        }
    }
    renderer.SetCompositor(nullptr);
    compositor.Shutdown();
    DestroyWindow(window);
    OleUninitialize();
    std::cout << "[SUMMARY] " << checks-failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
