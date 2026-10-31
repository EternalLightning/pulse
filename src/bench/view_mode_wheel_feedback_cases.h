// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution.

#pragma once
#include "../app/app_internal.h"
#include "../app/app_surface_scroll.h"
#include "../ui/animation_clock.h"
#include "animation_snapshot.h"

inline int pulse::ui::ItemReflowUiTest::RunWheelFeedback(const std::filesystem::path& output) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    wchar_t transport[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SNAPSHOT_STDOUT", transport, ARRAYSIZE(transport)) != 1 ||
        transport[0] != L'1') std::filesystem::create_directories(output);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = s.shot.active = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.scale = 1.0f;
    s.accentColor = HexColor(0x0078D4);
    s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"View-mode wheel feedback",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize isolated production-wheel compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.compositor.RecreateTextFormats(1.0f);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1.0f);
    s.renderer.SetDetailsPanelVisible(false);
    s.renderer.motion_enabled_ = true;
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring root = L"C:\\pulse-synthetic-wheel-feedback";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 400; ++i) {
        fs::DirEntry entry;
        entry.name = L"Synthetic document " + std::to_wstring(i) + L".txt";
        entry.full_path = root + L"\\" + entry.name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.size = 4096;
        entries->push_back(std::move(entry));
    }
    tab.current_path = root;
    tab.SetSnapshot(entries);
    tab.group_mode = GroupMode::None;
    tab.loading = false;
    constexpr int selected = 120;
    tab.SelectOnly(selected);
    const std::wstring key = (*entries)[selected].full_path + L"\n" + (*entries)[selected].name;
    const auto rect = D2D1::RectF(0, 0, 1100, 820);
    const auto theme = MakeTheme(false, s.accentColor);
    const auto close = [](D2D1_RECT_F a, D2D1_RECT_F b) {
        return std::fabs(a.left - b.left) < 0.1f && std::fabs(a.top - b.top) < 0.1f &&
            std::fabs(a.right - b.right) < 0.1f && std::fabs(a.bottom - b.bottom) < 0.1f;
    };
    const auto valid = [](D2D1_RECT_F bounds) {
        return bounds.right > bounds.left && bounds.bottom > bounds.top;
    };
    const auto frame = [&](uint64_t now, const std::wstring& label = {}) {
        const auto vm = BuildVm(s, false);
        auto* dc = s.compositor.Dc();
        dc->BeginDraw();
        dc->Clear(theme.bg);
        s.renderer.motion_enabled_ = true;
        s.renderer.surface_animating_ = false;
        s.renderer.settings_switch_now_ = now;
        ++s.renderer.settings_switch_frame_;
        s.renderer.UpdateBrushes(theme);
        s.renderer.painter_.BeginFrame(theme, false);
        {
            ThumbnailCache::FrameScope thumbnails(s.renderer.thumbnail_cache_);
            s.renderer.DrawPane(vm, rect, theme);
        }
        const HRESULT result = dc->EndDraw();
        if (FAILED(result)) std::printf("[D2D] wheel-feedback time=%llu HRESULT=%08lx\n",
            static_cast<unsigned long long>(now), static_cast<unsigned long>(result));
        check(SUCCEEDED(result), "production-wheel frame draws without Direct2D errors");
        if (!label.empty()) check(WriteAnimationSnapshot(s.compositor, output / label),
            "save production-wheel feedback frame");
    };
    const auto wheel = [&](UINT message, WORD modifiers, short delta) {
        const auto bounds = FocusedPaneRect(s);
        POINT point{static_cast<LONG>((bounds.left + bounds.right) * 0.5f),
            static_cast<LONG>((bounds.top + bounds.bottom) * 0.5f)};
        ClientToScreen(s.hwnd, &point);
        HandleMouseWheel(&s, s.hwnd, message, MAKEWPARAM(modifiers, static_cast<WORD>(delta)),
            MAKELPARAM(point.x, point.y));
    };
    const auto anchor_position = [&] {
        const auto vm = BuildVm(s, false);
        const auto bounds = FocusedPaneRect(s);
        const auto list = s.renderer.PaneListRect(vm.pane, bounds);
        const auto item = s.renderer.ItemRectInPane(vm.pane, bounds, vm.pane.ViewIndex(selected));
        const bool horizontal = tab.view_mode == ViewMode::List;
        const float travel = horizontal ? list.right - list.left - (item.right - item.left)
                                        : list.bottom - list.top - (item.bottom - item.top);
        const float start = horizontal ? item.left - list.left : item.top - list.top;
        return std::pair<float, bool>{travel > 0 ? start / travel : 0.0f,
            item.right > list.left && item.left < list.right && item.bottom > list.top && item.top < list.bottom};
    };
    for (const WORD modifiers : {static_cast<WORD>(MK_SHIFT), static_cast<WORD>(MK_CONTROL)}) {
        const std::wstring prefix = modifiers == MK_SHIFT ? L"shift-" : L"control-";
        std::printf("[CASE] production WM_MOUSEWHEEL modifiers=%04x\n", static_cast<unsigned>(modifiers));
        Reset(s.renderer);
        s.renderer.page_surface_.Reset();
        s.horizontalScrollMotion.Reset();
        CancelScrollAnimation(s);
        tab.view_mode = ViewMode::Details;
        tab.view_mode_direct = false;
        ++tab.view_generation;
        tab.scroll_x = 0;
        tab.scroll_y = 0;
        {
            const auto vm = BuildVm(s, false);
            const auto bounds = FocusedPaneRect(s);
            const auto list = s.renderer.PaneListRect(vm.pane, bounds);
            const auto item = s.renderer.ItemRectInPane(vm.pane, bounds, vm.pane.ViewIndex(selected));
            tab.scroll_y = std::max(0.0f, item.top - list.top -
                (list.bottom - list.top - (item.bottom - item.top)) * 0.35f);
        }
        s.scrollTargetY = tab.scroll_y;
        const auto baseline_anchor = anchor_position();
        frame(1000, prefix + L"details.png");
        const auto initial = ViewTransition(s.renderer).IconBounds(key);
        check(valid(initial) && baseline_anchor.second, "deeply scrolled selected file starts visible");

        wheel(WM_MOUSEWHEEL, modifiers, WHEEL_DELTA);
        check(tab.view_mode == ViewMode::List && tab.view_mode_direct,
            "modifier wheel enters List through production direct-manipulation command");
        const auto list_anchor = anchor_position();
        check(tab.selected_index == selected && list_anchor.second && tab.scroll_x > 0 && tab.scroll_y == 0 &&
            std::fabs(list_anchor.first - baseline_anchor.first) < 0.01f,
            "wheel view change preserves selected file and its viewport position on horizontal List axis");
        frame(1100);
        check(ViewTransition(s.renderer).Pending() && close(ViewTransition(s.renderer).IconBounds(key), initial),
            "modifier wheel starts per-icon motion despite direct-manipulation flag");
        check(!s.renderer.pane_surfaces_[0].pending, "wheel mode change does not add a whole-pane fade");
        frame(1150, prefix + L"list-moving.png");
        const auto sampled = ViewTransition(s.renderer).IconBounds(key);
        check(ViewTransition(s.renderer).Pending() && valid(sampled) && !close(sampled, initial),
            "wheel icon advances before next input");

        const auto round_trip_generation = tab.view_generation;
        wheel(WM_MOUSEWHEEL, modifiers, WHEEL_DELTA);
        wheel(WM_MOUSEWHEEL, modifiers, -WHEEL_DELTA);
        check(tab.view_mode == ViewMode::List && tab.view_generation == round_trip_generation + 2,
            "opposite wheel messages return to current animation target before any render");
        frame(1150, prefix + L"pending-round-trip.png");
        check(ViewTransition(s.renderer).Pending() && close(ViewTransition(s.renderer).IconBounds(key), sampled) &&
            ViewTransition(s.renderer).CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
            "pending round-trip preserves sampled visual without cancellation or target snap");
        const auto round_trip_anchor = anchor_position();
        check(tab.selected_index == selected && round_trip_anchor.second &&
            std::fabs(round_trip_anchor.first - list_anchor.first) < 0.01f,
            "pending opposite-wheel round-trip preserves selected viewport anchor");

        wheel(WM_MOUSEWHEEL, modifiers, WHEEL_DELTA);
        check(tab.view_mode == ViewMode::SmallIcons && tab.view_mode_direct,
            "second modifier wheel advances to SmallIcons while animation is active");
        const auto grid_anchor = anchor_position();
        check(tab.selected_index == selected && grid_anchor.second && tab.scroll_y > 0 && tab.scroll_x == 0 &&
            std::fabs(grid_anchor.first - list_anchor.first) < 0.01f,
            "rapid wheel retarget preserves anchor when returning to vertical scrolling");
        frame(1160, prefix + L"retarget-start.png");
        check(ViewTransition(s.renderer).Pending() && close(ViewTransition(s.renderer).IconBounds(key), sampled),
            "rapid wheel retarget begins at currently displayed icon bounds");
        frame(1164);
        const auto high_refresh_sample = ViewTransition(s.renderer).IconBounds(key);
        frame(1168);
        const auto next_sample = ViewTransition(s.renderer).IconBounds(key);
        check(valid(high_refresh_sample) && valid(next_sample) &&
            (!close(high_refresh_sample, sampled) || !close(next_sample, high_refresh_sample)),
            "retarget geometry advances at high-refresh sample intervals");
        frame(1160 + ViewModeTransition::kDuration + 1, prefix + L"settled.png");
        check(!ViewTransition(s.renderer).Pending() && !s.renderer.SurfaceAnimating(),
            "wheel motion settles and releases its frame request");

        const auto before_burst = ViewTransition(s.renderer).IconBounds(key);
        const auto burst_generation = tab.view_generation;
        wheel(WM_MOUSEWHEEL, modifiers, -WHEEL_DELTA);
        wheel(WM_MOUSEWHEEL, modifiers, -WHEEL_DELTA);
        check(tab.view_mode == ViewMode::Details && tab.view_generation == burst_generation + 2 &&
            !ViewTransition(s.renderer).Pending(),
            "wheel burst updates latest logical mode before drawing any intermediate view");
        frame(1500, prefix + L"burst-latest.png");
        check(ViewTransition(s.renderer).Pending() && close(ViewTransition(s.renderer).IconBounds(key), before_burst) &&
            ViewTransition(s.renderer).CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
            "batched wheel events animate directly from last displayed view to latest target");

        uint64_t now = 1500;
        for (int reversal = 0; reversal < 8; ++reversal) {
            now += 4;
            frame(now);
            const auto before_reversal = ViewTransition(s.renderer).IconBounds(key);
            const short delta = reversal % 2 == 0 ? WHEEL_DELTA : -WHEEL_DELTA;
            const auto expected = reversal % 2 == 0 ? ViewMode::List : ViewMode::Details;
            wheel(WM_MOUSEWHEEL, modifiers, delta);
            frame(now);
            check(tab.view_mode == expected && ViewTransition(s.renderer).Pending() &&
                close(ViewTransition(s.renderer).IconBounds(key), before_reversal) &&
                ViewTransition(s.renderer).CachedIcons() <= ViewModeTransition::kMaxIcons * 2,
                "8ms wheel reversal retargets sampled icon bounds with bounded retained pictures");
            const auto anchor = anchor_position();
            check(tab.selected_index == selected && anchor.second &&
                std::fabs(anchor.first - baseline_anchor.first) < 0.01f,
                "rapid wheel reversal keeps selected source anchored across scroll axes");
            now += 4;
            frame(now);
        }
        frame(now + ViewModeTransition::kDuration + 1, prefix + L"burst-settled.png");
        check(tab.view_mode == ViewMode::Details && !ViewTransition(s.renderer).Pending() &&
            !s.renderer.SurfaceAnimating() && ViewTransition(s.renderer).CachedIcons() <= ViewModeTransition::kMaxIcons,
            "stopping rapid wheel input settles latest mode and releases previous icon pictures");
        frame(now + ViewModeTransition::kDuration + 101);
        check(tab.view_mode == ViewMode::Details && !ViewTransition(s.renderer).Pending(),
            "settled wheel burst never replays queued intermediate view modes");
    }

    for (const WORD modifiers : {static_cast<WORD>(MK_SHIFT), static_cast<WORD>(MK_CONTROL)}) {
        Reset(s.renderer);
        SetViewMode(s, ViewMode::Details);
        const auto generation = tab.view_generation;
        wheel(WM_MOUSEWHEEL, modifiers, 2 * WHEEL_DELTA);
        check(tab.view_mode == ViewMode::SmallIcons && tab.view_generation == generation + 1,
            "multi-notch wheel packet commits only the latest view mode");
        wheel(WM_MOUSEWHEEL, modifiers, -3 * WHEEL_DELTA);
        check(tab.view_mode == ViewMode::Tiles,
            "fast reverse packet honors every wheel notch");
        SetViewMode(s, ViewMode::Details);
        const auto partial_generation = tab.view_generation;
        wheel(WM_MOUSEWHEEL, modifiers, 40);
        wheel(WM_MOUSEWHEEL, modifiers, 40);
        check(tab.view_mode == ViewMode::Details && tab.view_generation == partial_generation && s.viewWheelAccum == 80,
            "sub-notch deltas accumulate without repeated premature view changes");
        wheel(WM_MOUSEWHEEL, modifiers, 40);
        check(tab.view_mode == ViewMode::List && s.viewWheelAccum == 0,
            "high-resolution wheel deltas emit exactly one view step per notch");
        wheel(WM_MOUSEWHEEL, modifiers, 0);
        check(tab.view_mode == ViewMode::List && s.viewWheelAccum == 0,
            "zero wheel delta never advances the view");
    }

    for (const WORD modifiers : {static_cast<WORD>(0), static_cast<WORD>(MK_SHIFT), static_cast<WORD>(MK_CONTROL)}) {
        Reset(s.renderer);
        s.horizontalScrollMotion.Reset();
        tab.view_mode = ViewMode::List;
        tab.view_mode_direct = false;
        ++tab.view_generation;
        tab.scroll_y = 0;
        tab.scroll_x = 220.0f;
        const auto generation = tab.view_generation;
        wheel(WM_MOUSEHWHEEL, modifiers, WHEEL_DELTA);
        auto vm = BuildVm(s, false);
        TickAppSurfaceScroll(s, vm, AnimationTimeMs() + 1000);
        check(tab.view_mode == ViewMode::List && tab.view_generation == generation && tab.scroll_x > 220.0f &&
            tab.scroll_y == 0 && !ViewTransition(s.renderer).Pending(),
            "horizontal wheel retains horizontal scrolling even with view-cycle modifiers");
    }
    check(tab.snapshot == entries && entries->size() == 400,
        "wheel fixture preserves its synthetic snapshot without filesystem enumeration");
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    return failures;
}
