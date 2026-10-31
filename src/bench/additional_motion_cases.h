// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "animation_snapshot.h"
#include "banner_motion_cases.h"
#include "tooltip_motion_cases.h"
#include "../ui/ui_renderer_internal.h"

namespace pulse::ui {
struct AdditionalMotionUiTest {
    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            failures += !ok;
        };
        RunBannerMotionChecks(check);
        check(RunTooltipMotionChecks() == 0, "tooltip state checks pass");
        auto& r = s.renderer;
        for (const float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
            s.scale = scale;
            r.SetScale(scale);
            s.compositor.Resize(static_cast<int>(1200 * scale), static_cast<int>(760 * scale));
            s.compositor.RecreateTextFormats(scale);
            r.page_surface_.Reset();
            for (auto& surface : r.pane_surfaces_) surface.Reset();
            r.details_open_motion_ = {}; r.details_exit_commands_.reset();
            r.details_motion_window_width_ = 0;
            r.motion_enabled_ = true;
            r.SetDetailsPanelVisible(false);
            WindowViewModel vm;
            vm.dark = dark; vm.safe_mode = true; vm.details_motion_sampled = true;
            vm.pane.path = L"C:\\Motion";
            vm.pane.header_text = L"Motion";
            for (int i = 0; i < 8; ++i) {
                ListEntryView entry;
                entry.name = L"Folder " + std::to_wstring(i + 1);
                entry.path = vm.pane.path + L"\\" + entry.name; entry.is_dir = true;
                vm.pane.entries.push_back(entry);
            }
            const auto rect = D2D1::RectF(0, 0, 1200 * scale, 760 * scale);
            const auto draw = [&](const wchar_t* name) {
                auto* dc = s.compositor.Dc();
                dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0));
                r.Render(vm, rect, MakeTheme(dark, HexColor(0x0078D4)));
                const auto hr = dc->EndDraw();
                if (FAILED(hr)) std::printf("D2D HRESULT: %08lx\n", static_cast<unsigned long>(hr));
                check(SUCCEEDED(hr), "additional motion draws without Direct2D errors");
                if (name) {
                    const auto prefix = std::wstring(dark ? L"dark-" : L"light-") + (scale == 1 ? L"100-" : L"150-");
                    check(WriteAnimationSnapshot(s.compositor, root / (prefix + name + L".png")), "capture additional motion frame");
                }
            };
            r.TickDetailsPanelAnimation(1000, rect.right, true); draw(L"closed");
            const auto full_content = r.ContentRect(rect.right, rect.bottom);
            r.SetDetailsPanelVisible(true); vm.details_visible = true;
            r.TickDetailsPanelAnimation(2000, rect.right, true);
            check(r.details_open_motion_.Pending() && r.DetailsPanelWidth(rect.right) == 0, "opening panel starts without a width jump");
            r.TickDetailsPanelAnimation(2055, rect.right, true); draw(L"panel-opening");
            const auto opening = r.DetailsPanelRect(rect.right, rect.bottom);
            check(r.DetailsPanelWidth(rect.right) > 0 && r.DetailsPanelWidth(rect.right) < r.DetailsPanelTargetWidth(rect.right),
                "file list reserves intermediate panel width");
            check(opening.right > rect.right && std::abs(opening.right - opening.left - (r.DetailsPanelTargetWidth(rect.right) - r.Margin())) < 0.1f,
                "moving panel keeps full layout width instead of squeezing text");
            check(std::abs(r.ContentRect(rect.right, rect.bottom).right + r.Margin() - opening.left) < 0.1f,
                "file list and sliding panel share the same animated boundary");
            const auto resize_hit = r.HitTest(vm, rect, opening.left + scale, opening.top + 30 * scale);
            check(resize_hit.region == HitTestResult::DetailsResize, "panel hit testing follows its painted boundary");
            r.SetDetailsPanelVisible(false); vm.details_visible = false;
            r.TickDetailsPanelAnimation(2055, rect.right, true);
            check(std::abs(r.DetailsPanelRect(rect.right, rect.bottom).left - opening.left) < 0.1f,
                "panel reversal samples its current position");
            r.TickDetailsPanelAnimation(2100, rect.right, true); draw(L"panel-closing");
            check(r.HitTest(vm, rect, r.DetailsPanelRect(rect.right, rect.bottom).left + scale, opening.top + 30 * scale).region != HitTestResult::DetailsResize,
                "closing panel disables its resize interaction");
            r.TickDetailsPanelAnimation(2300, rect.right, true); draw(nullptr);
            check(r.DetailsPanelWidth(rect.right) == 0 && !r.details_open_motion_.Pending() && r.ContentRect(rect.right, rect.bottom).right == full_content.right,
                "closed panel releases width and animation frames");
            r.SetDetailsPanelVisible(true); vm.details_visible = true;
            r.TickDetailsPanelAnimation(2400, rect.right, true);
            r.TickDetailsPanelAnimation(2700, rect.right, false); draw(L"panel-open");
            check(!r.details_open_motion_.Pending(), "disabled panel motion settles immediately");
            r.TickDetailsPanelAnimation(2800, 800 * scale, true);
            check(r.DetailsPanelWidth(800 * scale) == 0 && !r.details_open_motion_.Pending(), "narrow layout hides the panel without a frame chain");
            r.SetDetailsPanelVisible(false); vm.details_visible = false;
            r.TickDetailsPanelAnimation(2900, rect.right, false); draw(nullptr);
            vm.pane.view_mode = ViewMode::MediumIcons; draw(nullptr);
            check(r.pane_surfaces_[0].pending, "changing view mode starts a content transition at the same path");
            Sleep(65); draw(L"view-middle");
            vm.pane.view_mode = ViewMode::Details; draw(nullptr);
            check(r.pane_surfaces_[0].pending, "rapid view-mode reversal preserves the previous visual");
            Sleep(230); draw(nullptr);
            check(!r.pane_surfaces_[0].pending, "view-mode transition terminates");
            vm.pane.banner_title = L"Search status";
            vm.pane.banner_message = L"Partial results are available.";
            vm.pane.network_search_root = L"\\\\server\\share";
            vm.pane.banner_expansion = 0.5f; draw(L"banner-half");
            const auto pane = r.ContentRect(rect.right, rect.bottom);
            const auto half_list = r.PaneListRect(vm.pane, pane);
            vm.pane.banner_expansion = 0;
            const auto no_banner_list = r.PaneListRect(vm.pane, pane);
            check(std::abs(half_list.top - no_banner_list.top - 18 * scale) < 0.1f, "list origin follows animated banner height");
            vm.pane.banner_expansion = 0.5f;
            check(r.HitTest(vm, rect, pane.right - 20 * scale, pane.top + 45 * scale).region != HitTestResult::NetworkSearchAdd,
                "transparent early banner has no clickable action");
            vm.pane.banner_expansion = 0.8f; draw(L"banner-revealing");
            const auto shown_action = r.HitTest(vm, rect, pane.right - 20 * scale, pane.top + 40 * scale + 5 * scale);
            const auto clipped_action = r.HitTest(vm, rect, pane.right - 20 * scale, pane.top + 40 * scale + 31 * scale);
            check(shown_action.region == HitTestResult::NetworkSearchAdd && clipped_action.region != HitTestResult::NetworkSearchAdd,
                "banner action accepts visible pixels and rejects collapsed pixels");
            vm.pane.banner_expansion = 0; draw(L"banner-hidden");
            check(r.HitTest(vm, rect, pane.right - 20 * scale, pane.top + 45 * scale).region != HitTestResult::NetworkSearchAdd,
                "fully collapsed banner has no clickable action");
            vm.tooltip_text = L"File details"; vm.tooltip_x = rect.right - 20 * scale;
            vm.tooltip_y = rect.bottom - 20 * scale; vm.hover_region = HitTestResult::Pane;
            draw(nullptr); Sleep(35); draw(L"tooltip-entering");
            Sleep(120); draw(nullptr);
            vm.tooltip_text.clear(); draw(nullptr); Sleep(35); draw(L"tooltip-leaving");
            Sleep(150); draw(nullptr);
            check(r.tooltip_text_.empty() && !r.tooltip_transition_.Pending(), "tooltip fade-out releases retained text and frame chain");
            auto& tab = *ActiveTab(s);
            tab.current_path = L"C:\\Motion";
            tab.ClearBanner(); tab.banner_animation = {};
            tab.banner_animation.Tick(tab.current_path, L"", L"", L"", 0, 1000, true);
            const auto list_without_banner = ListRect(s);
            tab.SetBannerTitle(app::BannerType::Other, L"Status"); tab.banner_message = L"Temporary status";
            tab.banner_animation.Tick(tab.current_path, tab.banner_title, tab.banner_message, L"", 0, 1100, true);
            tab.banner_animation.Tick(tab.current_path, tab.banner_title, tab.banner_message, L"", 0, 1190, true);
            const auto list_with_banner = ListRect(s);
            check(list_with_banner.top > list_without_banner.top && list_with_banner.top < list_without_banner.top + 36 * scale,
                "application list rectangle uses the current intermediate banner height");
            tab.ClearBanner();
            tab.banner_animation.Tick(tab.current_path, L"", L"", L"", 0, 1190, true);
            check(std::abs(ListRect(s).top - list_with_banner.top) < 0.1f,
                "application list rectangle retains outgoing banner space after logical removal");
            tab.banner_animation.Tick(tab.current_path, L"", L"", L"", 0, 1400, true);
            check(ListRect(s).top == list_without_banner.top, "application list rectangle restores its final origin after collapse");
        }
        return failures == 0;
    }
};
}
