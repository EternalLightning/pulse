// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "../ui/toolbar_layout.h"
#include "../ui/address_search_layout.h"
#include "../ui/home_layout.h"
#include "../ui/file_picker_commands.h"
#include "../common/resource_ids.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace {
int ColorPixels(pulse::ui::Compositor& compositor, D2D1_RECT_F bounds, D2D1_COLOR_F color, int tolerance = 8) {
    using namespace pulse::ui;
    auto* dc = compositor.Dc();
    ComPtr<ID2D1Image> image;
    ComPtr<ID2D1Bitmap1> target, readable;
    dc->GetTarget(&image);
    if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return -1;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat());
    if (FAILED(dc->CreateBitmap(target->GetPixelSize(), nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) return -1;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return -1;
    const auto size = target->GetPixelSize();
    const int left = std::clamp(static_cast<int>(std::ceil(bounds.left)), 0, static_cast<int>(size.width));
    const int right = std::clamp(static_cast<int>(std::floor(bounds.right)), left, static_cast<int>(size.width));
    const int top = std::clamp(static_cast<int>(std::ceil(bounds.top)), 0, static_cast<int>(size.height));
    const int bottom = std::clamp(static_cast<int>(std::floor(bounds.bottom)), top, static_cast<int>(size.height));
    int count = 0;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        const auto* pixel = mapped.bits + static_cast<size_t>(y) * mapped.pitch + x * 4;
        if (std::abs(pixel[0] - color.b * 255) < tolerance && std::abs(pixel[1] - color.g * 255) < tolerance &&
            std::abs(pixel[2] - color.r * 255) < tolerance) ++count;
    }
    readable->Unmap();
    return count;
}

void CheckMainUiColors(pulse::AppState& s, const std::filesystem::path& output,
                      const std::function<void(bool, const char*)>& check) {
    using namespace pulse;
    ui::HighContrastFrameScope contrast(true);
    auto theme = ui::MakeHighContrastTheme();
    theme.bg = theme.surface_title = theme.surface_sheet = theme.surface_card =
        theme.fill_input = theme.fill_input_focus = ui::HexColor(0x201020);
    theme.text = ui::HexColor(0xFFFFFF);
    theme.fill_hover = theme.fill_pressed = theme.fill_selected = theme.accent = ui::HexColor(0xFFFFFF);
    theme.text_selected = theme.accent_text = ui::HexColor(0x000000);
    theme.text_secondary = theme.text_disabled = ui::HexColor(0x808080);
    theme.accent_ink = ui::HexColor(0x00FFFF);
    theme.stroke_card = theme.stroke_divider = ui::HexColor(0x806080);
    theme.danger = ui::HexColor(0xFF0000);
    for (float scale : {1.0f, 1.5f}) for (int width : {1100, 500}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(720 * scale));
        const auto rect = D2D1::RectF(0, 0, width * scale, 720 * scale);
        auto vm = BuildVm(s, false);
        vm.dark = true;
        vm.window_effect = ui::WindowEffect::None;
        vm.backdrop_enabled = false;
        vm.background_image.clear();
        vm.pane_slots.clear();
        vm.pane.loading = true;
        vm.can_go_back = true;
        vm.can_go_forward = false;
        vm.can_group = true;
        const float left = s.renderer.EffectiveSidebarWidth(rect.right);
        const auto layout = ui::MakeToolbarLayout(rect.right, scale, s.renderer.TitleBarHeight(),
            s.renderer.Margin(), s.renderer.NewButtonWidthPx(rect.right - left < 600 * scale), left);
        const auto paint = [&](const wchar_t* name) {
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(vm, rect, theme);
            check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "render isolated main UI palette fixture");
            const auto file = std::wstring(name) + L"-" + std::to_wstring(width) + L"-" +
                std::to_wstring(static_cast<int>(scale * 100)) + L".png";
            check(s.compositor.SaveSnapshot((output / file).c_str()), "save main UI palette fixture");
        };
        const auto ink = [&](D2D1_RECT_F area, const char* label) {
            check(ColorPixels(s.compositor, area, theme.text_selected) > 3, label);
        };
        vm.hover_region = ui::HitTestResult::NavBack;
        paint(L"hc-navigation");
        ink(layout.navigation[0], "HC hovered navigation pairs white Highlight with black HighlightText");
        check(ColorPixels(s.compositor, layout.navigation[1], theme.text_disabled) > 3,
            "HC disabled navigation retains GrayText");
        check(ColorPixels(s.compositor, layout.navigation[2], theme.text_secondary) > 3,
            "HC ordinary navigation retains its normal foreground");
        vm.hover_region = ui::HitTestResult::ToolbarGroup;
        paint(L"hc-command");
        ink(layout.group, "HC independently drawn toolbar SVG uses HighlightText");
        vm.operation_count = 12;
        vm.operation_failed = true;
        vm.hover_region = ui::HitTestResult::OperationCenter;
        paint(L"hc-operation-active-failed");
        const auto badge = D2D1::RectF(layout.operations.right-28*scale, layout.operations.top,
            layout.operations.right+2*scale, layout.operations.top+14*scale);
        const auto failure = D2D1::RectF(layout.operations.right-8*scale, layout.operations.bottom-9*scale,
            layout.operations.right-2*scale, layout.operations.bottom-3*scale);
        ink(D2D1::RectF(badge.left, badge.top, badge.right, badge.top+10*scale),
            "active operation count remains legible while a failure is present");
        check(ColorPixels(s.compositor, failure, theme.danger) > 3 && badge.bottom < failure.top &&
            failure.right <= layout.operations.right && failure.bottom <= layout.operations.bottom,
            "simultaneous active count and failure dot are visible, separated and inside the operation control");
        vm.operation_count = 0;
        vm.operation_failed = false;
        vm.address_search_animation = 1.0f;
        vm.address_search_content = false;
        vm.address_search_options_open = true;
        vm.hover_region = ui::HitTestResult::AddressSearchOptions;
        const auto search = ui::LayoutAddressSearch(s.renderer.SearchBarRect(rect.right), scale);
        paint(L"hc-search");
        ink(search.options, "HC independently drawn search options glyph uses HighlightText");
        const float mode_x = (search.name.left + search.name.right)*0.5f;
        const float mode_y = (search.name.top + search.name.bottom)*0.5f;
        // Thin strokes at 100% DPI need not contain a fully covered black pixel.
        const auto mode_ink = D2D1::RectF(mode_x-8*scale,mode_y-8*scale,mode_x+8*scale,mode_y+8*scale);
        check(ColorPixels(s.compositor, mode_ink, theme.accent_text, 32) > 3,
            "HC selected compact search mode glyph uses accent text, including antialiased strokes");
        if (search.scope.right > search.scope.left) {
            vm.address_search_scope_open = true;
            vm.hover_region = ui::HitTestResult::AddressSearchScope;
            paint(L"hc-search-scope");
            ink(search.scope, "HC pressed compact search scope glyph uses HighlightText");
        }
        vm.address_search_animation = 0.0f;
        vm.pane.loading = false;
        vm.pane.is_home = true;
        vm.pane.network_discovery_disabled = true;
        vm.pane.home_cards = {{L"Library", L"", L"C:\\fixture", ui::kIconFolder, 0}};
        const auto home = ui::MakeHomeLayout(vm.pane, s.renderer.ContentRect(rect.right, rect.bottom), scale);
        vm.hover_pane_index = 0;
        vm.hover_control_index = 0;
        vm.hover_region = ui::HitTestResult::HomeGroup;
        paint(L"hc-home-group");
        ink(home.headings[0], "HC Home group disclosure and heading pair with HighlightText");
        vm.hover_region = ui::HitTestResult::HomeCard;
        paint(L"hc-home-card");
        ink(home.cards.front().rect, "HC Home card icon and label pair with HighlightText");
        vm.hover_region = ui::HitTestResult::HomeNetworkSettings;
        paint(L"hc-home-notice");
        ink(home.network_notice, "HC Home network notice text pairs with HighlightText");
        if (width == 1100) {
            s.renderer.SetDetailsPanelVisible(true);
            vm.details_visible = true;
            vm.details.has_selection = true;
            vm.details.is_dir = true;
            vm.details.name = L"Fixture";
            vm.details.preview_enabled = false;
            vm.details.collapsed_mask = 0x1Fu & ~(1u << 2);
            vm.hover_region = ui::HitTestResult::DetailsTagAdd;
            paint(L"hc-details-tag");
            const auto panel = s.renderer.DetailsPanelRect(rect.right, rect.bottom);
            ui::DetailsHitRects hit;
            ui::LayoutDetailsPanel(panel, scale, vm.details, s.compositor.DwriteFactory(),
                s.compositor.SmallFormat(), &s.compositor, 0, hit);
            ink(hit.tag_add, "HC details action uses public button selected foreground");
            check(std::abs(hit.tag_add.bottom-hit.tag_add.top-32*scale) < 0.01f,
                "details tag action shares the public 32 DIP button height");
            check(s.renderer.HitTest(vm, rect, (hit.tag_add.left+hit.tag_add.right)*0.5f,
                (hit.tag_add.top+hit.tag_add.bottom)*0.5f).region == ui::HitTestResult::DetailsTagAdd,
                "details action drawing and hit testing share the same bounds");
            vm.details.is_dir = false;
            vm.details.cloud_online = true;
            vm.details.preview_enabled = true;
            vm.details.preview_only = true;
            vm.hover_region = ui::HitTestResult::DetailsCloudDownload;
            paint(L"hc-details-download");
            ui::LayoutDetailsPanel(panel, scale, vm.details, s.compositor.DwriteFactory(),
                s.compositor.SmallFormat(), &s.compositor, ui::DetailsPreviewHeight(panel, scale), hit);
            ink(hit.cloud_download, "HC cloud download action uses public button selected foreground");
            check(std::abs(hit.cloud_download.bottom-hit.cloud_download.top-32*scale) < 0.01f &&
                s.renderer.HitTest(vm, rect, (hit.cloud_download.left+hit.cloud_download.right)*0.5f,
                    (hit.cloud_download.top+hit.cloud_download.bottom)*0.5f).region == ui::HitTestResult::DetailsCloudDownload,
                "cloud download uses a 32 DIP button with matching hit bounds");
            s.renderer.SetDetailsPanelVisible(false);
            vm.details_visible = false;
        }
        {
            const auto contrast_theme = theme;
            ui::HighContrastFrameScope normal(false);
            vm.pane.is_home = false;
            vm.pane.loading = true;
            vm.operation_count = 12;
            vm.operation_failed = true;
            vm.hover_region = 0;
            for (const bool dark : {false, true}) {
                vm.dark = dark;
                theme = ui::MakeTheme(dark, s.accentColor);
                paint(dark ? L"dark-operation-active-failed" : L"light-operation-active-failed");
                const int count_pixels = ColorPixels(s.compositor, badge, theme.accent_text);
                const int failure_pixels = ColorPixels(s.compositor, failure, theme.danger);
                if (count_pixels <= 3 || failure_pixels <= 3)
                    std::printf("badge diagnostics: width=%d scale=%.2f dark=%d count_pixels=%d failure_pixels=%d\n",
                        width, scale, dark, count_pixels, failure_pixels);
                check(count_pixels > 3 && failure_pixels > 3,
                    "ordinary light and dark palettes retain both active count and failure indicator");
            }
            theme = contrast_theme;
        }
    }
}
}

int RunGroupToolbarUiTest(std::string_view test_mode) {
    using namespace pulse;
    const bool order_only = test_mode == "--group-order-only";
    const bool colors_only = test_mode == "--main-ui-colors-only";
    const bool icons_only = test_mode == "--command-icons-only";
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto output = std::filesystem::absolute(order_only
        ? L"docs/validation/group-order-20261030" : colors_only
        ? L"bench_data/main-ui-colors" : icons_only
        ? L"bench_data/command-icons" : L"bench_data/group-toolbar-ui");
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
    if (icons_only) {
        s.scale = 1.0f;
        s.renderer.SetScale(s.scale);
        s.compositor.RecreateTextFormats(s.scale);
        s.compositor.Resize(1100, 720);
        for (const bool dark : {true, false}) {
            auto vm = BuildVm(s, false);
            vm.dark = dark;
            vm.window_effect = ui::WindowEffect::None;
            vm.backdrop_enabled = false;
            vm.pane_slots.clear();
            vm.pane.loading = true;
            vm.toolbar_commands_enabled = std::array{true, true, true, true, true, true};
            const auto theme = ui::MakeTheme(dark, s.accentColor);
            const auto suffix = dark ? L"-dark.png" : L"-light.png";
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            s.renderer.Render(vm, D2D1::RectF(0, 0, 1100, 720), theme);
            check(SUCCEEDED(dc->EndDraw()) &&
                s.compositor.SaveSnapshot((output / (std::wstring(L"toolbar") + suffix)).c_str()),
                "capture enabled main toolbar commands");
            ui::FluentMenu menu;
            if (!menu.Create(s.hwnd, &s.compositor, 1.0f)) { check(false, "create command icon menu"); break; }
            menu.SetTheme(dark, s.accentColor);
            const auto capture = [&](const wchar_t* name, auto items) {
                check(menu.SaveDebugSnapshot((output / (std::wstring(name) + suffix)).c_str(), std::move(items)),
                    "capture built-in command menu");
            };
            capture(L"context-item", app::BuildItemMenu(false, L""));
            capture(L"context-background", app::BuildBackgroundMenu(false, false, L""));
            capture(L"picker-menu", ui::PickerContextMenu({true, true, 1, true}));
        }
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        return failures ? 1 : 0;
    }
    if (colors_only) {
        CheckMainUiColors(s, output, check);
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
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
        for (float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) for (bool dark : {false, true}) {
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
                check(layout.sort.right - layout.sort.left == layout.group.right - layout.group.left,
                    "sort and group buttons share a width in text and icon modes");
                if (layout.sort.right - layout.sort.left >= 60 * scale) {
                    ui::fluent::Painter painter(&s.compositor);
                    painter.SetScale(scale);
                    painter.BeginFrame(ui::MakeTheme(dark, s.accentColor));
                    check(painter.MeasureButtonWidth(l10n::Get(l10n::StringId::ToolbarSort), L"\xE8CB", true) <=
                        layout.sort.right - layout.sort.left,
                        "localized sort label, icon and chevron fit without clipping");
                    check(painter.MeasureButtonWidth(l10n::Get(l10n::StringId::ToolbarGroup), L"\xE8FD", true) <=
                        layout.group.right - layout.group.left,
                        "localized group label, supplied icon and chevron fit without clipping");
                    check(painter.MeasureButtonWidth(l10n::Get(l10n::StringId::ToolbarFilter), L"\xE71C") <=
                        layout.filter.right - layout.filter.left,
                        "localized filter label and icon fit without clipping");
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
    const int result = RunGroupToolbarUiTest(argc == 2 ? std::string_view(argv[1]) : std::string_view{});
    OleUninitialize();
    return result;
}
