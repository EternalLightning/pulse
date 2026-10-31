// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../app/app_hover.h"
#include "animation_snapshot.h"

namespace pulse::ui {
struct InteractionFeedbackUiTest {
    static bool RunDisclosure(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
        };
        auto& r = s.renderer;
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            r.SetScale(scale); s.compositor.RecreateTextFormats(scale);
            s.compositor.Resize(static_cast<int>(1200 * scale), static_cast<int>(600 * scale));
            r.hover_tracks_.clear(); r.motion_enabled_ = true;
            WindowViewModel vm; vm.dark = dark;
            SidebarGroup group; group.id = 42; group.header = L"Workspace";
            SidebarItem item; item.label = L"This PC"; item.path = L"C:\\Disclosure";
            item.expandable = true; item.icon_glyph = L"\xE770";
            group.items.push_back(item); vm.sidebar.push_back(group);
            const std::wstring key = L"sidebar-disclosure:42:C:\\Disclosure";
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            int widget = 0;
            const auto draw = [&](uint64_t now, const wchar_t* name) {
                r.settings_switch_now_ = now; ++r.settings_switch_frame_; r.surface_animating_ = false;
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                r.UpdateBrushes(theme);
                const auto bounds = D2D1::RectF(0, 0, 1200 * scale, 600 * scale);
                if (widget == 1) r.DrawDetailsPanel(vm, bounds, theme);
                else if (widget == 2) r.DrawSettingsCore(vm, bounds, theme);
                else r.DrawSidebar(vm, bounds, theme);
                D2D1_MATRIX_3X2_F transform; dc->GetTransform(&transform);
                check(transform._11 == 1 && transform._12 == 0 && transform._21 == 0 && transform._22 == 1,
                    "disclosure drawing restores the transform for following controls");
                check(SUCCEEDED(dc->EndDraw()), "sidebar disclosure frame renders without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") +
                        std::to_wstring(static_cast<int>(scale * 100)) + L"-";
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")),
                        "capture sidebar disclosure rotation frame");
                }
            };
            vm.sidebar[0].collapsed = true; vm.sidebar[0].expansion = 0;
            draw(1000, L"section-closed");
            vm.sidebar[0].collapsed = false; vm.sidebar[0].expansion = 0.5f;
            draw(1100, L"section-middle");
            vm.sidebar[0].expansion = 1;
            draw(1200, L"section-open");
            check(r.hover_tracks_.contains(key) && r.hover_tracks_.at(key).animation.value == 0,
                "new collapsed tree item initializes at the correct arrow endpoint");
            vm.sidebar[0].items[0].expanded = true;
            draw(1300, nullptr);
            check(r.hover_tracks_.at(key).animation.value == 0 && r.SurfaceAnimating(),
                "opening a sidebar tree item schedules rotation from its current angle");
            draw(1360, L"item-middle");
            const float middle = r.hover_tracks_.at(key).animation.value;
            check(middle > 0 && middle < 1, "sidebar tree item has an intermediate rotation angle");
            vm.sidebar[0].items[0].expanded = false; draw(1360, nullptr);
            check(r.hover_tracks_.at(key).animation.value == middle,
                "rapid disclosure reversal preserves the current angle");
            draw(1540, L"item-closed");
            check(r.hover_tracks_.at(key).animation.value == 0 && !r.SurfaceAnimating(),
                "closed disclosure reaches its endpoint and releases frame scheduling");
            vm.sidebar[0].items[0].expanded = true; r.motion_enabled_ = false;
            draw(1600, L"motion-disabled");
            check(r.hover_tracks_.at(key).animation.value == 1 && !r.SurfaceAnimating(),
                "disabled motion snaps the disclosure directly to its expanded endpoint");
            r.motion_enabled_ = true;
            widget = 1; r.SetDetailsPanelVisible(true);
            r.TickDetailsPanelAnimation(1700, 1200 * scale, false);
            vm.details.has_selection = true; vm.details.name = L"Disclosure sample.txt";
            vm.details.preview_enabled = false; vm.details.collapsed_mask = 31;
            draw(1800, L"details-closed");
            vm.details.collapsed_mask = 30; draw(1900, nullptr); draw(1960, L"details-middle");
            check(r.hover_tracks_.contains(L"details-disclosure:0") &&
                r.hover_tracks_.at(L"details-disclosure:0").animation.value > 0 &&
                r.hover_tracks_.at(L"details-disclosure:0").animation.value < 1,
                "details section draws an intermediate disclosure angle");
            draw(2100, L"details-open");
            vm.details.preview_enabled = true; vm.details.preview_only = true; vm.details.preview_expansion = 0.5f;
            draw(2150, L"preview-middle");
            vm.details.preview_enabled = false; vm.details.preview_only = false; vm.details.preview_expansion = 0.0f;
            widget = 2; vm.settings_page = 1; vm.settings_expanded = 0;
            draw(2200, L"settings-closed");
            vm.settings_expanded = 2; draw(2300, nullptr); draw(2360, L"settings-middle");
            check(r.hover_tracks_.contains(L"settings-disclosure:1:1") &&
                r.hover_tracks_.at(L"settings-disclosure:1:1").animation.value > 0 &&
                r.hover_tracks_.at(L"settings-disclosure:1:1").animation.value < 1,
                "settings maintenance section draws an intermediate disclosure angle");
            draw(2500, L"settings-open");
            r.SetDetailsPanelVisible(false);
        }
        return failures == 0;
    }
    static bool RunHomeHover(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
        };
        auto& r = s.renderer;
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            s.compositor.Resize(static_cast<int>(900 * scale), static_cast<int>(660 * scale));
            s.compositor.RecreateTextFormats(scale); r.SetScale(scale); r.motion_enabled_ = true;
            WindowViewModel vm; vm.dark = dark; vm.hover_pane_index = 0;
            PaneViewModel pane; pane.is_home = true; pane.network_discovery_disabled = true;
            pane.home_cards.push_back({L"Documents", L"", LR"(C:\Fixture\Documents)", L"", 0});
            pane.home_cards.push_back({L"Local disk", L"100 GB free", LR"(C:\)", L"", 1, 0.4f});
            const auto bounds = D2D1::RectF(0, 0, 900 * scale, 660 * scale);
            const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(static_cast<int>(scale * 100));
            for (int target = 0; target < 3; ++target) {
                r.hover_tracks_.clear();
                const auto key = target == 0 ? std::wstring(LR"(home:0:card:0:C:\Fixture\Documents)") :
                    target == 1 ? std::wstring(L"home:0:group:0") : std::wstring(L"home:0:network-notice");
                const auto draw = [&](uint64_t now, bool hot, bool capture) {
                    r.settings_switch_now_ = now; ++r.settings_switch_frame_; r.surface_animating_ = false;
                    vm.hover_region = hot ? (target == 0 ? HitTestResult::HomeCard :
                        target == 1 ? HitTestResult::HomeGroup : HitTestResult::HomeNetworkSettings) : 0;
                    vm.hover_control_index = 0;
                    auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(MakeTheme(dark, HexColor(0x0078D4)).bg);
                    r.DrawHome(vm, pane, bounds, 0, MakeTheme(dark, HexColor(0x0078D4)));
                    check(SUCCEEDED(dc->EndDraw()), "home hover frame renders without Direct2D errors");
                    if (capture) check(WriteAnimationSnapshot(s.compositor, root / (prefix + L"-target-" +
                        std::to_wstring(target) + L"-" + std::to_wstring(now) + L".png")), "capture home hover frame");
                    return r.hover_tracks_.at(key).animation.value;
                };
                check(draw(1000, false, false) == 0, "home element starts at its resting appearance");
                check(draw(1100, true, false) == 0 && r.SurfaceAnimating(), "home hover schedules entrance frames");
                const float middle = draw(1150, true, true);
                check(middle > 0 && middle < 1, "home background and border share an intermediate progress");
                check(draw(1150, false, false) == middle, "rapid home hover exit retains current progress");
                check(draw(1330, false, true) == 0 && !r.SurfaceAnimating(), "home hover exit settles and stops requesting frames");
                r.motion_enabled_ = false;
                check(draw(1400, true, false) == 1, "home hover respects disabled motion");
                r.motion_enabled_ = true;
            }
        }
        return failures == 0;
    }
    static bool RunHover(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        auto& r = s.renderer;
        const auto bounds = D2D1::RectF(24, 24, 160, 60);
        r.hover_tracks_.clear(); r.motion_enabled_ = true;
        r.settings_switch_now_ = 1000;
        check(r.ControlHoverOpacity(bounds, false) == 0, "control starts without a hover flash");
        r.settings_switch_now_ = 1100;
        check(r.ControlHoverOpacity(bounds, true) == 0 && r.SurfaceAnimating(), "hover entrance schedules frames from zero opacity");
        r.settings_switch_now_ = 1150;
        const float middle = r.ControlHoverOpacity(bounds, true);
        check(middle > 0 && middle < 1, "control has an intermediate hover opacity");
        check(r.ControlHoverOpacity(bounds, false) == middle, "pointer exit reverses from its current opacity");
        const auto other = D2D1::RectF(180, 24, 316, 60);
        check(r.ControlHoverOpacity(other, true) == 0, "moving to another control starts an independent entrance");
        r.settings_switch_now_ = 1320;
        check(r.ControlHoverOpacity(bounds, false) == 0 && r.ControlHoverOpacity(other, true) == 1,
            "old control fades out while the new one fades in");
        r.motion_enabled_ = false;
        check(r.ControlHoverOpacity(bounds, true) == 1 && r.ControlHoverOpacity(bounds, false) == 0,
            "disabled animations preserve immediate hover feedback");
        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            s.compositor.Resize(static_cast<int>(520 * scale), static_cast<int>(260 * scale));
            s.compositor.RecreateTextFormats(scale); r.SetScale(scale);
            r.hover_tracks_.clear(); r.motion_enabled_ = true;
            auto scaled = [scale](float l, float t, float right, float b) {
                return D2D1::RectF(l * scale, t * scale, right * scale, b * scale);
            };
            for (const int phase : {0, 1, 2, 3, 4, 5}) {
                r.settings_switch_now_ = 2000 + (phase == 0 ? 0 : phase == 1 ? 100 : phase == 2 ? 150 :
                    phase == 3 ? 230 : phase == 4 ? 300 : 470);
                ++r.settings_switch_frame_;
                const bool hot = phase > 0 && phase < 4;
                const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                r.painter_.BeginFrame(theme);
                fluent::ButtonSpec button; button.bounds = scaled(24, 24, 160, 60);
                button.text = L"Button"; button.kind = fluent::ButtonKind::Transparent; button.state.hovered = hot;
                r.painter_.DrawButton(button);
                fluent::SidebarSectionHeaderSpec section;
                section.bounds = scaled(24, 80, 220, 116); section.text = L"Group"; section.state.hovered = hot;
                r.painter_.DrawSidebarSectionHeader(section);
                fluent::SplitterSpec splitter; splitter.bounds = scaled(24, 140, 480, 156);
                splitter.vertical = false; splitter.state.hovered = hot; r.painter_.DrawSplitter(splitter);
                check(SUCCEEDED(dc->EndDraw()), "hover controls render without Direct2D errors");
                const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                    std::to_wstring(static_cast<int>(scale * 100)) + L"-hover-" + std::to_wstring(phase) + L".png";
                check(WriteAnimationSnapshot(s.compositor, root / name), "capture hover entrance and exit frame");
            }
        }
        return failures == 0;
    }
    static bool RunListHover(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        auto& r = s.renderer;
        for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            s.scale = scale; r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1200 * scale), static_cast<int>(500 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.motion_enabled_ = true; r.hover_tracks_.clear(); r.painting_pane_ = 0;
            WindowViewModel vm;
            vm.dark = dark;
            vm.pane.path = L"C:\\Hover";
            vm.pane.view_mode = ViewMode::Details;
            for (int i = 0; i < 6; ++i) {
                ListEntryView entry;
                entry.name = L"Folder " + std::to_wstring(i + 1);
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.is_dir = true;
                vm.pane.entries.push_back(entry);
            }
            auto bounds = D2D1::RectF(0, 0, 1200 * scale, 500 * scale);
            const auto theme = MakeTheme(dark, HexColor(0x0078D4));
            const auto row_key = L"0:" + vm.pane.path + L":" + vm.pane.entries[0].path +
                L":" + vm.pane.entries[0].name + L":0";
            const auto control_key = [](const D2D1_RECT_F& rc) {
                return L"control:0:" + std::to_wstring(rc.left) + L":" + std::to_wstring(rc.top) +
                    L":" + std::to_wstring(rc.right) + L":" + std::to_wstring(rc.bottom);
            };
            const auto address_key = control_key(r.AddressBarRect(bounds.right));
            const auto search_key = control_key(r.SearchBarRect(bounds.right));
            const auto value = [&](const std::wstring& key) {
                const auto found = r.hover_tracks_.find(key);
                return found == r.hover_tracks_.end() ? -1.0f : found->second.animation.value;
            };
            const auto draw = [&](uint64_t now, int row, HitTestResult::Region region, const wchar_t* name) {
                ++r.settings_switch_frame_; r.settings_switch_now_ = now; r.surface_animating_ = false;
                vm.pane.hover_index = row; vm.hover_region = region; vm.hover_control_index = row;
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(theme.bg);
                r.UpdateBrushes(theme); r.painter_.BeginFrame(theme);
                r.DrawToolbar(vm, bounds, theme);
                r.DrawList(vm.pane, 24 * scale, 150 * scale, bounds.right - 48 * scale, 300 * scale,
                    theme, region, row);
                check(SUCCEEDED(dc->EndDraw()), "list and toolbar hover frame renders without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") +
                        std::to_wstring(static_cast<int>(scale * 100)) + L"-";
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")),
                        "capture list buttons and field hover frame");
                }
            };
            draw(1000, -1, HitTestResult::None, L"rest");
            draw(1100, 0, HitTestResult::RowStar, L"row-enter");
            check(value(row_key) == 0 && value(row_key + L":star-reveal") == 0 &&
                value(row_key + L":more-reveal") == 0 && r.SurfaceAnimating(),
                "row highlight and action icons enter from zero instead of appearing immediately");
            draw(1145, 0, HitTestResult::RowStar, L"row-middle");
            check(value(row_key) > 0 && value(row_key) < 0.9f &&
                value(row_key + L":star-reveal") > 0 && value(row_key + L":star-reveal") < 1 &&
                value(row_key + L":new-tab-reveal") > 0 && value(row_key + L":more-reveal") < 1,
                "row, star, new-tab and more expose intermediate opacity during the visible transition");
            draw(1300, 0, HitTestResult::RowStar, L"row-end");
            check(value(row_key + L":star-hot") == 1 && !r.SurfaceAnimating(),
                "star button hover settles and stops requesting animation frames");
            draw(1350, 0, HitTestResult::RowMore, nullptr);
            draw(1395, 0, HitTestResult::RowMore, L"button-retarget");
            check(value(row_key + L":star-hot") > 0 && value(row_key + L":star-hot") < 1 &&
                value(row_key + L":more-hot") > 0 && value(row_key + L":more-hot") < 1,
                "star exit and more entrance animate independently while the row stays hovered");
            draw(1550, 0, HitTestResult::RowMore, nullptr);
            draw(1600, -1, HitTestResult::None, nullptr);
            draw(1645, -1, HitTestResult::None, L"row-leaving");
            check(value(row_key + L":star-reveal") > 0 && value(row_key + L":star-reveal") < 1 &&
                value(row_key + L":more-reveal") > 0 && value(row_key + L":more-reveal") < 1,
                "leaving the row retains both icons for their fade-out");
            draw(1800, -1, HitTestResult::None, nullptr);
            check(value(row_key + L":star-reveal") == 0 && !r.SurfaceAnimating(),
                "row exit clears the action icons and releases animation scheduling");
            draw(2000, -1, HitTestResult::AddressBar, nullptr);
            draw(2045, -1, HitTestResult::AddressBar, L"address-middle");
            check(value(address_key) > 0 && value(address_key) < 1,
                "production address field uses an intermediate hover background");
            draw(2200, -1, HitTestResult::AddressBar, nullptr);
            draw(2250, -1, HitTestResult::AddressSearch, nullptr);
            draw(2295, -1, HitTestResult::AddressSearch, L"search-middle");
            check(value(address_key) > 0 && value(address_key) < 1 &&
                value(search_key) > 0 && value(search_key) < 1,
                "address field exit and search field entrance both use the animated field painter");
            draw(2450, -1, HitTestResult::AddressSearch, L"search-end");
            draw(2500, -1, HitTestResult::None, nullptr);
            draw(2680, -1, HitTestResult::None, nullptr);
            check(!r.SurfaceAnimating(), "settled fields leave no hover repaint loop");
            r.motion_enabled_ = false;
            draw(2800, 0, HitTestResult::RowStar, nullptr);
            check(value(row_key) == 1 && value(row_key + L":star-reveal") == 1 &&
                value(row_key + L":more-reveal") == 1 && !r.SurfaceAnimating(),
                "disabled motion keeps immediate row and button feedback without animation work");
            bounds.right = 760 * scale;
            s.compositor.Resize(static_cast<int>(bounds.right), static_cast<int>(bounds.bottom));
            draw(2900, 0, HitTestResult::RowMore, L"narrow");
        }
        s.scale = 1; r.SetScale(1); r.motion_enabled_ = false;
        s.compositor.Resize(1200, 700); s.compositor.RecreateTextFormats(1);
        SetWindowPos(s.hwnd, nullptr, 0, 0, 1200, 700, SWP_NOACTIVATE | SWP_NOZORDER);
        auto& tab = *ActiveTab(s);
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(80);
        for (size_t i = 0; i < entries->size(); ++i) {
            auto& entry = (*entries)[i];
            entry.name = L"Scroll item " + std::to_wstring(i) + L".txt";
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
        }
        tab.SetSnapshot(entries); tab.loading = false; tab.scroll_y = 0;
        tab.view_mode = ViewMode::Details; tab.pending_generation = 0;
        PrepareViewState(s);
        auto vm = BuildVm(s, false);
        const auto pane = FocusedPaneRect(s);
        const auto list = r.PaneListRect(vm.pane, pane);
        const POINT point{static_cast<LONG>(list.left + 80), static_cast<LONG>(list.top + r.RowHeight() * 0.5f)};
        s.mouseLeaveTracked = true; s.hoverPoint = point;
        RefreshStationaryHover(s, vm);
        const int original = s.hoverRow;
        check(original >= 0, "stationary-pointer fixture starts over a real file row");
        s.tooltipText = L"Old row tooltip";
        POINT screen = point; ClientToScreen(s.hwnd, &screen);
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
        Render(s);
        check(s.hoverPoint.x == point.x && s.hoverPoint.y == point.y && s.hoverRow > original &&
            s.tooltipText.empty() && s.hitTestVm.pane.hover_index == s.hoverRow,
            "real wheel plus production paint retargets the stationary pointer and clears the old tooltip immediately");
        const auto since = s.hoverSince;
        Render(s);
        check(s.hoverSince == since, "repainting an unchanged hover does not restart its tooltip delay");
        const int previous = s.hoverRow;
        r.motion_enabled_ = true;
        StartSmoothScroll(s, r.RowHeight() * 3);
        s.scrollLastUpdateTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(90);
        Render(s);
        check(s.hoverRow > previous && s.hitTestVm.pane.hover_index == s.hoverRow,
            "subsequent animated scroll frames also recompute the row beneath a stationary pointer");
        const auto hit = r.HitTest(s.hitTestVm, D2D1::RectF(0, 0, 1200, 700),
            static_cast<float>(point.x), static_cast<float>(point.y));
        auto changed_identity = hit; changed_identity.path = L"C:\\ReplacedRow";
        s.tooltipText = L"Replaced item tooltip";
        check(UpdateHoverFromHit(s, changed_identity, point) && s.tooltipText.empty(),
            "a replaced item at the same row index cannot retain the previous tooltip identity");
        HandleMouseLeave(&s, s.hwnd, WM_MOUSELEAVE, 0, 0);
        vm = BuildVm(s, false); RefreshStationaryHover(s, vm);
        check(s.hoverRow == -1 && s.hoverRegion == 0,
            "scroll repaint after pointer leave does not resurrect an old hover");
        return failures == 0;
    }
    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        auto& r = s.renderer;
        r.motion_enabled_ = true; r.hover_tracks_.clear();
        r.settings_switch_now_ = 1000;
        check(r.SelectionOpacity(L"fixture", false) == 0, "unselected row initializes without a flash");
        r.settings_switch_now_ = 1100;
        check(r.SelectionOpacity(L"fixture", true) == 0 && r.SurfaceAnimating(), "click schedules the selected-background entrance");
        r.settings_switch_now_ = 1160;
        const float middle = r.SelectionOpacity(L"fixture", true);
        check(middle > 0 && middle < 1 && r.SelectionOpacity(L"fixture", false) == middle,
            "rapid deselection preserves the current background opacity");
        r.settings_switch_now_ = 1280;
        check(r.SelectionOpacity(L"fixture", false) == 0, "selection exit reaches its exact endpoint");
        r.motion_enabled_ = false;
        check(r.SelectionOpacity(L"fixture", true) == 1, "disabled motion selects immediately");

        for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
            s.scale = scale; r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1200 * scale), static_cast<int>(760 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.motion_enabled_ = true; r.hover_tracks_.clear();
            r.SetDetailsPanelVisible(false); r.details_open_motion_ = {};
            r.TickDetailsPanelAnimation(1000, 1200 * scale, false);
            r.page_surface_.Reset();
            for (auto& surface : r.pane_surfaces_) surface.Reset();
            WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.details_motion_sampled = true;
            vm.pane.path = L"C:\\Feedback"; vm.pane.header_text = L"Feedback";
            vm.pane.view_mode = ViewMode::Details;
            for (int i = 0; i < 8; ++i) {
                ListEntryView entry;
                entry.name = L"Image " + std::to_wstring(i + 1) + L".jpg";
                entry.path = vm.pane.path + L"\\" + entry.name;
                entry.attrs = FILE_ATTRIBUTE_NORMAL;
                vm.pane.entries.push_back(entry);
            }
            const auto bounds = D2D1::RectF(0, 0, 1200 * scale, 760 * scale);
            const auto draw = [&](const wchar_t* name) {
                auto* dc = s.compositor.Dc(); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                r.Render(vm, bounds, MakeTheme(dark, HexColor(0x0078D4)));
                const auto result = dc->EndDraw();
                if (FAILED(result)) std::printf("D2D HRESULT: %08lx\n", static_cast<unsigned long>(result));
                check(SUCCEEDED(result), "feedback frame renders without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + (scale == 1 ? L"100-" : L"150-");
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")), "capture feedback frame");
                }
            };
            draw(L"selection-empty");
            vm.pane.selected_index = 1; vm.pane.selected_count = 1;
            draw(nullptr);
            check(r.SurfaceAnimating() && vm.pane.IsRowSelected(1), "logical selection is immediate while its background animates");
            Sleep(40); draw(L"selection-row-middle");
            Sleep(140); draw(L"selection-row-end");
            check(!r.SurfaceAnimating(), "row selection stops requesting frames after settling");
            vm.pane.selected_index = 4; draw(nullptr);
            Sleep(40); draw(L"selection-row-retarget");
            Sleep(140); draw(nullptr);
            vm.pane.view_mode_direct = true;
            for (const auto mode : {ViewMode::MediumIcons, ViewMode::LargeIcons, ViewMode::ExtraLargeIcons, ViewMode::Details}) {
                vm.pane.view_mode = mode; draw(nullptr);
                check(!r.pane_surfaces_[0].pending, "wheel view changes never start repeated full-pane fades");
            }
            vm.pane.view_mode = ViewMode::MediumIcons; draw(L"wheel-icons");
            vm.pane.selected_index = 2; draw(nullptr);
            Sleep(40); draw(L"selection-icon-middle");
            Sleep(140); draw(nullptr);
            vm.pane.selected_index = -1; vm.pane.selected_count = 0; draw(nullptr);
            Sleep(40); draw(L"selection-icon-leaving");
            Sleep(140); draw(nullptr);
            check(!r.SurfaceAnimating(), "icon selection exit releases its frame chain");
            vm.pane.view_mode_direct = false; vm.pane.view_mode = ViewMode::Details; draw(nullptr);
            check(r.view_mode_transitions_[0].Pending(), "menu view change animates individual icons");
            Sleep(50); draw(L"menu-middle"); Sleep(220); draw(nullptr);
            check(!r.view_mode_transitions_[0].Pending(), "menu view change settles normally");
            vm.pane.view_mode_direct = true; vm.pane.path += L"\\Child"; draw(nullptr);
            check(r.pane_surfaces_[0].pending, "wheel policy does not suppress a subsequent folder navigation");
            Sleep(230); draw(nullptr);
            r.motion_enabled_ = false; vm.pane.selected_index = 3; vm.pane.selected_count = 1; draw(nullptr);
            check(!r.SurfaceAnimating(), "reduced motion removes selection and view animation work");
        }
        r.motion_enabled_ = true;
        auto& tab = *ActiveTab(s);
        SetViewMode(s, ViewMode::Details);
        PrepareViewState(s);
        auto vm = BuildVm(s, false);
        const auto body = r.ContentRect(static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        POINT point{static_cast<LONG>(body.left + 100 * s.scale), static_cast<LONG>(body.top + 100 * s.scale)};
        ClientToScreen(s.hwnd, &point);
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(point.x, point.y));
        check(tab.view_mode_direct && tab.view_mode != ViewMode::Details,
            "production Ctrl-wheel path marks direct manipulation");
        app::FillPaneViewModel(vm.pane, *s.pane, &s.places);
        check(vm.pane.view_mode_direct, "pane projection carries wheel presentation policy");
        SetViewMode(s, ViewMode::Details);
        check(!tab.view_mode_direct, "menu command restores the animated presentation policy");
        return failures == 0;
    }
};
}
