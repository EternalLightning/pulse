// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/pane_animations.h"
#include "animation_snapshot.h"
#include "../common/localization.h"
#include "../ui/ui_renderer_internal.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <algorithm>
#include <tuple>

namespace pulse { void UpdateSidebarPinDrag(AppState& s, int my); }

namespace pulse::ui {
struct StagingTrayMotionTest {
    static void SetMotion(MainRenderer& renderer, bool enabled) { renderer.motion_enabled_ = enabled; }
};
}

int QuickAccessDragTest() {
    using namespace pulse;
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"quick access fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1100, 780, nullptr, nullptr, nullptr, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = L"C:\\quick-access-fixture";
    ActiveTab(s)->git_root = L"C:\\quick-access-fixture\\project";
    s.sidebarHiddenMask = ((1u << app::kSidebarSectionCount) - 1u) &
        ~(1u << static_cast<int>(app::SidebarSectionId::QuickAccess));
    for (const auto& [name, path, builtin] : std::vector<std::tuple<const wchar_t*, const wchar_t*, app::BuiltinQuickAccess>>{
             {L"最近使用", L"pulse:recent", app::BuiltinQuickAccess::Recent},
             {L"桌面", L"C:\\quick-access-fixture\\desktop", app::BuiltinQuickAccess::Desktop},
             {L"下载", L"C:\\quick-access-fixture\\downloads", app::BuiltinQuickAccess::Downloads},
             {L"回收站", L"pulse:recycle", app::BuiltinQuickAccess::RecycleBin}}) {
        app::SidebarEntry entry;
        entry.label = name; entry.path = path; entry.glyph = L"\xE8B7";
        entry.builtin = static_cast<int>(builtin);
        s.sidebar.quick_access.push_back(std::move(entry));
    }
    const std::wstring pin = L"C:\\quick-access-fixture\\pinned";
    s.places.SetQuickAccessPinned({pin}, true);
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const int section = static_cast<int>(app::SidebarSectionId::QuickAccess);
    const auto drag = [&](const std::wstring& path, bool to_front) {
        const auto vm = BuildVm(s, false);
        const int group = app::SidebarSectionIndex(vm, section);
        if (group < 0) { check(false, "quick access group exists"); return; }
        const auto& items = vm.sidebar[static_cast<size_t>(group)].items;
        const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) {
            return _wcsicmp(fs::NormalizePath(item.path).c_str(), fs::NormalizePath(path).c_str()) == 0;
        });
        D2D1_RECT_F source{}, target{};
        const float width = static_cast<float>(s.compositor.Width());
        const float height = static_cast<float>(s.compositor.Height());
        if (found == items.end() || !s.renderer.SidebarRowRect(vm, width, height, section,
                static_cast<int>(found - items.begin()), &source) ||
            !s.renderer.SidebarRowRect(vm, width, height, section,
                to_front ? 0 : static_cast<int>(items.size() - 1), &target)) {
            check(false, "real layout exposes drag source and target"); return;
        }
        const POINT point{static_cast<LONG>(source.left + 100 * s.scale),
            static_cast<LONG>((source.top + source.bottom) / 2)};
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        check(s.pinDragPending && !s.pinDragActive && s.pinDragPath == found->path,
            "real row press arms drag for built-ins, project and pinned folders");
        // Enter the drag state directly instead of injecting desktop mouse input.
        s.pinDragActive = true;
        const int y = static_cast<int>(to_front ? target.top - 1 : target.bottom + 1);
        UpdateSidebarPinDrag(s, y);
        check(s.pinGapVisible && s.pinDragToIndex == (to_front ? 0 : static_cast<int>(items.size())),
            "insertion line measures the entire quick-access list");
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, y));
        const auto after = BuildVm(s, false);
        const auto& moved = after.sidebar[static_cast<size_t>(app::SidebarSectionIndex(after, section))].items;
        check(_wcsicmp(fs::NormalizePath((to_front ? moved.front() : moved.back()).path).c_str(),
                fs::NormalizePath(path).c_str()) == 0 && !s.pinDragPending && !s.pinDragActive,
            "release commits full-list ordering and clears drag state");
    };
    for (const float scale : {1.0f, 1.25f, 1.5f}) {
        s.scale = scale; s.renderer.SetScale(scale); s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(1100 * scale), static_cast<UINT>(780 * scale));
        s.places.quick_access_order.clear();
        for (const auto& entry : s.sidebar.quick_access) drag(entry.path, false);
        drag(ActiveTab(s)->git_root, false);
        drag(pin, true);
    }
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto output = std::filesystem::path(executable).parent_path().parent_path() / L"bench_data" / L"quick-access-order";
    std::error_code error;
    std::filesystem::create_directories(output, error);
    for (const bool dark : {false, true}) {
        s.darkMode = dark;
        const auto vm = BuildVm(s, false);
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        auto* dc = s.compositor.Dc();
        dc->BeginDraw(); dc->Clear(theme.bg);
        s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height())), theme);
        const bool rendered = SUCCEEDED(dc->EndDraw());
        const auto snapshot = output / (dark ? L"dark-150.png" : L"light-150.png");
        check(!error && rendered && s.compositor.SaveSnapshot(snapshot.c_str()),
            "render and save reordered quick-access rows in both themes");
    }
    const HWND hwnd = s.hwnd;
    s.hwnd = nullptr; owned.reset(); DestroyWindow(hwnd);
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--quick-access") {
        const int result = QuickAccessDragTest();
        OleUninitialize();
        return result;
    }
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    ui::fluent::Painter painter;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        painter.SetScale(scale);
        for (float height : {20.0f, 26.0f, 32.0f}) {
            for (bool dot : {false, true}) {
                const auto row = D2D1::RectF(10 * scale, 20 * scale, 210 * scale, (20 + height) * scale);
                const auto icon = painter.SidebarItemIconRect(row, dot);
                check(std::abs(icon.right - icon.left - 16 * scale) < 0.001f &&
                    std::abs(icon.bottom - icon.top - 16 * scale) < 0.001f &&
                    std::abs(icon.top + icon.bottom - row.top - row.bottom) < 0.001f &&
                    icon.top >= row.top && icon.bottom <= row.bottom,
                    "sidebar icon keeps centered 16 DIP square across row heights and DPI");
            }
        }
    }
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1000, 700,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(s.hwnd && s.compositor.Init(s.hwnd), "initialize isolated real layout/hit fixture");
    if (!s.compositor.Dc()) return 1;
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1.0f);
    s.compositor.RecreateTextFormats(1.0f);
    s.window_tabs.NewTab(L"C:\\PulseSidebarTrayFixture");
    s.pane = s.window_tabs.Active()->panes.front().get();
    // Nonexistent synthetic paths are allowed in the tray; no file writes/deletes.
    s.tray.Collect({L"C:\\PulseSidebarTrayFixture\\a", L"C:\\PulseSidebarTrayFixture\\b",
        L"C:\\PulseSidebarTrayFixture\\c", L"C:\\PulseSidebarTrayFixture\\d"}, false);
    double now = TrayAnimationNow();
    auto settle = [&]() {
        for (int i = 0; i < 90; ++i) TickTrayDeck(s, now += 16.0);
        (void)BuildVm(s, false);
        s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 180;
        (void)BuildVm(s, false);
    };
    settle();
    auto find_hit = [&](ui::HitTestResult::Region region) {
        const auto vm = BuildVm(s, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()));
        const auto panel = s.renderer.StagingTrayRect(vm, bounds.right, bounds.bottom);
        for (float y = panel.top; y < panel.bottom; y += 1.0f)
            for (float x = panel.left; x < panel.right; x += 1.0f)
                if (s.renderer.HitTest(vm, bounds, x, y).region == region)
                    return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
        return POINT{-1, -1};
    };
    auto press = [&](POINT pt, bool twice) {
        const LPARAM pos = MAKELPARAM(pt.x, pt.y);
        if (twice) HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, pos);
        else HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, pos);
    };
    if (argc == 2 && std::wstring_view(argv[1]) == L"--tray-animation") {
        s.ctxMenuPrefs.persist = false;
        app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](app::SettingsEffect effect) { ApplySettingsEffects(s, effect); };
        s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
        ui::StagingTrayMotionTest::SetMotion(s.renderer, true);
        const auto output = std::filesystem::absolute(L"bench_data/review-changes/tray-animation");
        std::error_code error;
        std::filesystem::create_directories(output, error);
        for (const float scale : {1.0f, 1.5f}) for (const float width : {56.0f, 160.0f, 224.0f}) {
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.renderer.SetSidebarWidthDip(width);
            s.compositor.RecreateTextFormats(scale);
            const float window_width = width == 56.0f ? 800.0f : 1000.0f;
            s.compositor.Resize(static_cast<UINT>(window_width * scale), static_cast<UINT>(700 * scale));
            const auto bounds = D2D1::RectF(0, 0, window_width * scale, 700 * scale);
            const auto sidebar = s.renderer.SidebarRect(bounds.right, bounds.bottom);
            check(ui::SidebarRailLayout(sidebar.right - sidebar.left, scale) == (width == 56.0f),
                "animation fixture exercises the requested rail or expanded sidebar");
            s.appPrefs.staging_tray_enabled = true;
            settle();
            const auto open_vm = BuildVm(s, false);
            const auto open_panel = s.renderer.StagingTrayRect(open_vm, bounds.right, bounds.bottom);
            const auto open_bar = ui::SidebarScrollbarSpec(open_vm, sidebar, scale);
            s.settings.ToggleUi(36);
            auto vm = BuildVm(s, false);
            check(!vm.staging_tray_enabled && vm.staging_tray_presence > 0.99f &&
                s.stagingTrayAnimation.Pending(), "disabling starts an exit from the visible tray");
            s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 60;
            vm = BuildVm(s, false);
            std::vector<ui::SidebarSlot> slots;
            ui::LayoutSidebar(vm, sidebar, scale, slots);
            const auto panel = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
                return slot.kind == ui::SidebarSlot::TrayPanel;
            });
            const auto bar = ui::SidebarScrollbarSpec(vm, sidebar, scale);
            check(panel != slots.end() && panel->rc.top > open_panel.top && panel->rc.top < sidebar.bottom &&
                std::abs((panel->rc.bottom - panel->rc.top) - (open_panel.bottom - open_panel.top)) < 0.01f &&
                (width == 56.0f || bar.viewport.bottom > open_bar.viewport.bottom),
                "exit slides the full tray and smoothly returns its scroll space");
            check(s.renderer.StagingTrayRect(vm, bounds.right, bounds.bottom).right == 0 &&
                find_hit(ui::HitTestResult::TrayItemRemove).x < 0,
                "exiting tray cannot accept drops or hidden card hits");
            if (panel != slots.end()) {
                bool inert = true;
                for (float y = std::max(sidebar.top, panel->rc.top); y < sidebar.bottom; y += 4 * scale)
                    for (float x = panel->rc.left; x < panel->rc.right; x += 4 * scale) {
                        const auto hit = s.renderer.HitTest(vm, bounds, x, y).region;
                        inert &= hit != ui::HitTestResult::TrayRelease && hit != ui::HitTestResult::TrayClear &&
                            hit != ui::HitTestResult::TrayCard && hit != ui::HitTestResult::TrayItemRemove &&
                            hit != ui::HitTestResult::TrayPrev && hit != ui::HitTestResult::TrayNext;
                    }
                check(inert, "all outgoing tray actions become inert immediately");
            }
            for (const bool dark : {false, true}) {
                vm.dark = dark;
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                auto* dc = s.compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg); s.renderer.Render(vm, bounds, theme);
                const auto result = dc->EndDraw();
                const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                    std::to_wstring(static_cast<int>(width)) + L"-" +
                    std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                check(!error && SUCCEEDED(result) && WriteAnimationSnapshot(s.compositor, output / name),
                    "render an intermediate tray frame across themes, widths and DPI");
            }
            s.settings.ToggleUi(36);
            settle();
            s.settings.ToggleUi(36);
            (void)BuildVm(s, false);
            s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 60;
            const float before_reverse = BuildVm(s, false).staging_tray_presence;
            s.settings.ToggleUi(36);
            vm = BuildVm(s, false);
            check(vm.staging_tray_enabled && vm.staging_tray_presence <= before_reverse + 0.001f &&
                vm.staging_tray_presence > 0.0f && s.stagingTrayAnimation.target == 1.0f,
                "rapid reversal resumes from the currently sampled tray pose");
            s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 180;
            vm = BuildVm(s, false);
            check(vm.staging_tray_presence == 1.0f && !s.stagingTrayAnimation.Pending(),
                "opening settles and stops requesting animation frames");
            s.settings.ToggleUi(36);
            (void)BuildVm(s, false);
            s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 180;
            vm = BuildVm(s, false);
            ui::LayoutSidebar(vm, sidebar, scale, slots);
            check(vm.staging_tray_presence == 0.0f && !s.stagingTrayAnimation.Pending() &&
                std::none_of(slots.begin(), slots.end(), [](const auto& slot) {
                    return slot.kind == ui::SidebarSlot::TrayPanel || slot.kind == ui::SidebarSlot::TrayRelease ||
                        slot.kind == ui::SidebarSlot::TrayClear;
                }), "completed exit removes tray geometry and stops animation");
            s.settings.ToggleUi(36);
            vm = BuildVm(s, false);
            check(vm.staging_tray_presence == 0.0f && s.stagingTrayAnimation.Pending(),
                "opening from hidden begins at the invisible pose");
            s.stagingTrayAnimation.started = app::PaneAnimationTimeMs() - 60;
            vm = BuildVm(s, false);
            check(vm.staging_tray_presence > 0.0f && vm.staging_tray_presence < 1.0f,
                "opening exposes intermediate poses");
            const auto visible_panel = s.renderer.StagingTrayRect(vm, bounds.right, bounds.bottom);
            check(visible_panel.top >= sidebar.top && visible_panel.bottom <= sidebar.bottom,
                "opening tray drop and wheel bounds stay inside its visible sidebar area");
        }
        for (const bool high_contrast : {false, true}) {
            ui::StagingTrayMotionTest::SetMotion(s.renderer, high_contrast);
            s.shot_high_contrast = high_contrast;
            for (const bool enabled : {false, true}) {
                s.appPrefs.staging_tray_enabled = enabled;
                const auto vm = BuildVm(s, false);
                check(vm.staging_tray_presence == (enabled ? 1.0f : 0.0f) && !s.stagingTrayAnimation.Pending(),
                    "disabled motion and high contrast snap directly without a frame loop");
            }
        }
        const HWND hwnd = s.hwnd;
        s.hwnd = nullptr; owned.reset(); DestroyWindow(hwnd); OleUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--tray-settings") {
        s.ctxMenuPrefs.persist = false;
        app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](app::SettingsEffect effect) { ApplySettingsEffects(s, effect); };
        s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, std::move(callbacks));
        app::AppPrefs restored;
        check(restored.FromJson(L"{\"tray_icon_size\":56}") && restored.staging_tray_enabled,
            "legacy icon-size preferences keep the tray enabled at its fixed size");
        const auto output = std::filesystem::absolute(L"bench_data/review-changes/tray-settings");
        std::error_code error;
        std::filesystem::create_directories(output, error);
        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            s.compositor.Resize(static_cast<UINT>(1000 * scale), static_cast<UINT>(700 * scale));
            for (const float width : {56.0f, 160.0f, 224.0f}) {
                const float window_width = width == 56.0f ? 800.0f : 1000.0f;
                s.compositor.Resize(static_cast<UINT>(window_width * scale), static_cast<UINT>(700 * scale));
                const auto bounds = D2D1::RectF(0, 0, window_width * scale, 700 * scale);
                s.renderer.SetSidebarWidthDip(width);
                for (const bool empty : {false, true}) {
                    s.tray.Clear();
                    if (!empty) s.tray.Collect({L"C:\\PulseSidebarTrayFixture\\retained",
                        L"C:\\PulseSidebarTrayFixture\\second"}, false);
                    s.appPrefs.staging_tray_enabled = true;
                    settle();
                    std::wstring items_before;
                    s.tray.ToJson(items_before);
                    auto open_vm = BuildVm(s, false);
                    const auto open_panel = s.renderer.StagingTrayRect(open_vm, bounds.right, bounds.bottom);
                    const auto sidebar = s.renderer.SidebarRect(bounds.right, bounds.bottom);
                    const auto open_scrollbar = ui::SidebarScrollbarSpec(open_vm, sidebar, scale);
                    check(open_panel.right > open_panel.left && open_vm.tray_deck.thumb_dip == 48.0f,
                        "enabled tray keeps its original fixed thumbnail size");
                    const bool rail = ui::SidebarRailLayout(sidebar.right - sidebar.left, scale);
                    if (!rail) {
                        check(std::abs(open_panel.bottom - open_panel.top - 164.0f * scale) < 0.01f,
                            "tray restores the standard fixed panel height");
                        if (!empty) {
                            const auto release = ui::fluent::StagingTrayActionRect(open_panel, scale);
                            check(find_hit(ui::HitTestResult::TrayRelease).x >= 0 &&
                                std::abs(release.right - release.left - (width == 224.0f ? 72.0f : 22.0f) * scale) < 0.01f,
                                "release keeps its text at standard width and fits narrow sidebars");
                        }
                        const auto former_close = s.renderer.HitTest(open_vm, bounds,
                            open_panel.right - 12 * scale, open_panel.top + 17 * scale);
                        check(former_close.region == (empty ? ui::HitTestResult::None : ui::HitTestResult::TrayRelease),
                            "former close position contains only the release action or empty header");
                    }
                    for (const bool enabled : {true, false}) {
                        if (!enabled) s.settings.ToggleUi(36);
                        settle();
                        auto vm = BuildVm(s, false);
                        const auto panel = s.renderer.StagingTrayRect(vm, bounds.right, bounds.bottom);
                        check(vm.staging_tray_enabled == enabled && restored.FromJson(s.appPrefs.ToJson()) &&
                            restored.staging_tray_enabled == enabled &&
                            s.appPrefs.ToJson().find(L"tray_icon_size") == std::wstring::npos,
                            "tray switch round-trips without retaining obsolete icon size");
                        if (!enabled) {
                            std::vector<ui::SidebarSlot> slots;
                            ui::LayoutSidebar(vm, sidebar, scale, slots);
                            bool hidden = panel.left == panel.right && panel.top == panel.bottom;
                            for (const auto& slot : slots)
                                hidden &= slot.kind != ui::SidebarSlot::TrayPanel &&
                                    slot.kind != ui::SidebarSlot::TrayRelease && slot.kind != ui::SidebarSlot::TrayClear;
                            check(hidden && find_hit(ui::HitTestResult::TrayItemRemove).x < 0,
                                "disabled tray has no panel, rail entry, actions or hidden hit targets");
                            const auto bar = ui::SidebarScrollbarSpec(vm, sidebar, scale);
                            check(rail || bar.viewport.bottom > open_scrollbar.viewport.bottom + 150 * scale,
                                "disabled tray returns its space to sidebar scrolling");
                            const int offset = s.trayDeckOffset;
                            POINT point{static_cast<LONG>((open_panel.left + open_panel.right) / 2),
                                static_cast<LONG>((open_panel.top + open_panel.bottom) / 2)};
                            ClientToScreen(s.hwnd, &point);
                            HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA),
                                MAKELPARAM(point.x, point.y));
                            check(s.trayDeckOffset == offset, "wheel cannot rotate disabled tray cards");
                        }
                        for (const bool dark : {false, true}) {
                            vm.dark = dark;
                            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                            auto* dc = s.compositor.Dc();
                            dc->BeginDraw();
                            dc->Clear(theme.bg);
                            s.renderer.Render(vm, bounds, theme);
                            const auto result = dc->EndDraw();
                            const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                                (enabled ? L"enabled-" : L"disabled-") + (empty ? L"empty-" : L"full-") +
                                std::to_wstring(static_cast<int>(width)) + L"-" +
                                std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                            check(!error && SUCCEEDED(result) && WriteAnimationSnapshot(s.compositor, output / name),
                                "render tray visibility across themes, narrow widths and DPI");
                        }
                    }
                    s.settings.ToggleUi(36);
                    std::wstring items_after;
                    s.tray.ToJson(items_after);
                    check(s.appPrefs.staging_tray_enabled && items_after == items_before,
                        "re-enabling the tray preserves all staged items");
                }
            }
        }
        s.scale = 1;
        s.renderer.SetScale(1);
        s.renderer.SetSidebarWidthDip(224);
        s.compositor.RecreateTextFormats(1);
        s.compositor.Resize(1100, 900);
        OpenSettingsTab(s, 9);
        const auto bounds = D2D1::RectF(0, 0, 1100, 900);
        auto vm = BuildVm(s, false);
        auto layout = ui::MakeSettingsLayout(vm, bounds, 1, s.renderer.TitleBarHeight(), 28, nullptr);
        const auto row = layout.staging_tray_row;
        const POINT toggle{static_cast<LONG>(row.right - 35), static_cast<LONG>((row.top + row.bottom) / 2)};
        const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(toggle.x), static_cast<float>(toggle.y));
        check(hit.region == ui::HitTestResult::SettingsToggle && hit.index == 36,
            "settings contains the enable switch in place of size choices");
        press(toggle, false);
        check(!s.appPrefs.staging_tray_enabled && !BuildVm(s, false).staging_tray_enabled,
            "native settings click immediately hides the staging tray");
        press(toggle, false);
        check(s.appPrefs.staging_tray_enabled, "second native settings click restores the tray");
        for (const bool dark : {false, true}) {
            vm = BuildVm(s, false);
            vm.dark = dark;
            const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
            auto* dc = s.compositor.Dc();
            dc->BeginDraw(); dc->Clear(theme.bg); s.renderer.Render(vm, bounds, theme);
            check(SUCCEEDED(dc->EndDraw()) && WriteAnimationSnapshot(s.compositor,
                output / (dark ? L"settings-dark.png" : L"settings-light.png")),
                "render settings enable switch in both themes");
        }
        const HWND hwnd = s.hwnd;
        s.hwnd = nullptr;
        owned.reset();
        DestroyWindow(hwnd);
        OleUninitialize();
        return failures ? 1 : 0;
    }
    const auto next = find_hit(ui::HitTestResult::TrayNext);
    const auto prev = find_hit(ui::HitTestResult::TrayPrev);
    check(next.x >= 0 && prev.x >= 0, "real layout exposes both pager buttons");
    press(next, false);
    press(next, true);
    check(TrayStackTop(s) == 2, "rapid next DOWN/DBLCLK advances twice without waiting");
    press(prev, false);
    press(prev, true);
    check(TrayStackTop(s) == 0, "rapid previous DOWN/DBLCLK advances twice without waiting");
    settle();
    const auto top = TrayDeckEntries(s.tray, 0, 1).front().item->path;
    ThrowTrayTop(s, 1.0f, 25.0f, 5.0f);
    const double start = s.trayCards.at(top).motion_started;
    TickTrayDeck(s, start + 140);
    check(s.trayCards.at(top).motion == AppState::TrayMotion::ThrowBack,
        "throw starts returning within 140ms");
    TickTrayDeck(s, start + 360);
    check(s.trayCards.at(top).motion == AppState::TrayMotion::None,
        "throw motion finishes within 360ms");
    s.trayDeckOffset = 0;
    settle();
    const auto close = find_hit(ui::HitTestResult::TrayItemRemove);
    check(close.x >= 0, "real layout exposes the top card X");
    const auto before = s.trayCards.at(top);
    press(close, false);
    TickTrayDeck(s, now += 16);
    check(TrayItemTotalCount(s.tray) == 3 && s.trayCards.at(top).ghost &&
        s.trayCards.at(top).exit == AppState::TrayExit::Fade && s.trayPuffs.empty(),
        "X removes only one item and creates a fade without smoke");
    TickTrayDeck(s, now += 70);
    const auto& faded = s.trayCards.at(top);
    check(faded.opacity < before.opacity && faded.opacity > 0 && faded.fly == before.fly &&
        faded.dx == before.dx && faded.dy == before.dy && faded.angle == before.angle &&
        faded.shrink == before.shrink && faded.motion == AppState::TrayMotion::None,
        "single dismissal changes opacity only, without flight rotation or shrink");
    TickTrayDeck(s, now += 80);
    check(!s.trayCards.contains(top), "dismissed ghost is gone within 150ms");
    settle();
    const auto remaining = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)), 1).front();
    const auto path = remaining.item->path;
    ThrowTrayTop(s, -1.0f, -40, 8);
    const auto moving = s.trayCards.at(path);
    MarkTrayExit(s, {path}, false);
    s.tray.RemoveItem(static_cast<size_t>(remaining.batch), static_cast<size_t>(remaining.sub));
    TickTrayDeck(s, now += 16);
    TickTrayDeck(s, now += 70);
    const auto& stopped = s.trayCards.at(path);
    check(stopped.motion == AppState::TrayMotion::None && stopped.exit == AppState::TrayExit::Fade &&
        stopped.dx == moving.dx && stopped.dy == moving.dy && stopped.fly == moving.fly &&
        stopped.angle == moving.angle, "dismissal freezes an in-progress fling at its current pose");
    // Native renderer snapshots use isolated in-memory sidebar data, never preferences.
    settle();
    wchar_t executable[MAX_PATH]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    check(executable_length > 0 && executable_length < ARRAYSIZE(executable),
        "resolve snapshot output relative to the fixture executable");
    const auto snapshot_dir = std::filesystem::path(executable).parent_path().parent_path() /
        L"bench_data" / L"review-changes" / L"feedback-ui";
    std::error_code snapshot_error;
    std::filesystem::create_directories(snapshot_dir, snapshot_error);
    check(!snapshot_error, "create isolated feedback snapshot directory");
    auto snapshot_vm = BuildVm(s, false);
    snapshot_vm.sidebar.clear();
    ui::SidebarGroup group;
    group.header = L"快速访问";
    for (const auto& [label, folder] : {
        std::pair{L"Pulse", L"E:\\Projects\\pulse"},
        std::pair{L"测试程序", L"E:\\Projects\\TestPrograms"},
        std::pair{L"源代码", L"E:\\Projects\\pulse\\src"},
        std::pair{L"界面", L"E:\\Projects\\pulse\\src\\ui"}}) {
        ui::SidebarItem item;
        item.label = label;
        item.path = folder;
        item.use_path_icon = true;
        item.icon_glyph = L"\uE8B7";
        item.fallback_text = L"D";
        item.icon_color = ui::HexColor(0xD6A02D);
        group.items.push_back(std::move(item));
    }
    snapshot_vm.sidebar.push_back(std::move(group));
    snapshot_vm.sidebar_scroll = 0;
    for (bool dark : {false, true}) {
        snapshot_vm.dark = dark;
        const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
        bool rendered = true;
        // Path icons resolve asynchronously; warm up bounded frames before capture.
        for (int frame = 0; frame < 20; ++frame) {
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(theme.bg);
            s.renderer.Render(snapshot_vm, D2D1::RectF(0, 0, 1000, 700), theme);
            rendered = SUCCEEDED(dc->EndDraw()) && rendered;
            if (frame < 19) Sleep(50);
        }
        const auto snapshot = snapshot_dir / (dark ? L"sidebar-tray-dark-100.png" :
            L"sidebar-tray-light-100.png");
        check(rendered && !snapshot_error && s.compositor.SaveSnapshot(snapshot.c_str()),
            dark ? "save native 100% dark sidebar/tray snapshot" :
                   "save native 100% light sidebar/tray snapshot");
        wprintf(L"[SNAPSHOT] %ls\n", snapshot.c_str());
    }
    const HWND hwnd = s.hwnd;
    s.hwnd = nullptr;
    owned.reset();
    DestroyWindow(hwnd);
    OleUninitialize();
    return failures ? 1 : 0;
}
