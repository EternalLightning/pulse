// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../app/app_internal.h"

inline int pulse::ui::ItemReflowUiTest::RunPaneLayoutTransition() {
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = false;
    s.showDetailsPanel = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Pane layout transition fixture",
        WS_POPUP, -30000, -30000, 1100, 820, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize pane layout transition compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.compositor.RecreateTextFormats(1.0f);
    auto& renderer = s.renderer;
    renderer.SetCompositor(&s.compositor);
    renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    while (Panes(s).size() < 4) Panes(s).push_back(std::make_unique<app::Pane>());
    for (size_t i = 0; i < Panes(s).size(); ++i) {
        auto& tab = *Panes(s)[i]->ActiveTab();
        tab.current_path = L"C:\\pulse-layout-fixture-" + std::to_wstring(i + 1);
        tab.SetSnapshot(std::make_shared<const std::vector<fs::DirEntry>>());
        tab.loading = false;
    }

    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    const auto content = renderer.ContentRect(bounds.right, bounds.bottom);
    const RECT crop{static_cast<LONG>(std::ceil(content.left)) + 4,
        static_cast<LONG>(std::ceil(content.top)) + 4,
        static_cast<LONG>(content.right) - 4, static_cast<LONG>(content.bottom) - 4};
    const auto theme = MakeTheme(false, HexColor(0x0078D4));
    const auto frame = [&](uint64_t now, bool animate = true, bool high_contrast = false) {
        const HighContrastFrameScope contrast(high_contrast);
        renderer.motion_enabled_ = animate;
        renderer.surface_animating_ = false;
        renderer.settings_switch_now_ = now;
        ++renderer.settings_switch_frame_;
        auto* dc = s.compositor.Dc();
        dc->BeginDraw();
        dc->Clear(theme.bg);
        renderer.UpdateBrushes(theme);
        renderer.painter_.BeginFrame(theme, high_contrast);
        ThumbnailCache::FrameScope thumbnails(renderer.thumbnail_cache_);
        renderer.DrawPane(BuildVm(s, false), bounds, theme);
        if (FAILED(dc->EndDraw())) return std::vector<uint32_t>{};
        return ReadScrollPixels(s.compositor, crop);
    };

    uint64_t now = 1000;
    for (const int command : {app::CmdLayoutTwoVertical, app::CmdLayoutTwoHorizontal, app::CmdLayoutFourGrid}) {
        DispatchMenuCommand(s, command);
        const auto split_pixels = frame(now);
        const auto divider = HitTestVm(s).splitters.front().hit_rect;
        DispatchMenuCommand(s, app::CmdLayoutSingle);
        const auto& single = HitTestVm(s);
        const auto hit = renderer.HitTest(single, bounds,
            (divider.left + divider.right) / 2, (divider.top + divider.bottom) / 2);
        check(single.pane_slots.size() == 1 && single.splitters.empty() && s.splitterNodes.empty() &&
            hit.pane_index == 0 && hit.region != HitTestResult::Splitter,
            "single-pane command updates hit geometry immediately");
        const auto start = frame(now + 10);
        check(!start.empty() && start == split_pixels && renderer.SurfaceAnimating(),
            "collapse starts from the painted split and requests another frame");
        const auto middle = frame(now + 30);
        check(!middle.empty() && middle != start && renderer.pane_layout_surface_.pending,
            "collapse produces a distinct intermediate frame");
        DispatchMenuCommand(s, command);
        const auto reversed = frame(now + 30);
        check(reversed == middle && renderer.pane_layout_surface_.pending,
            "rapid reversal starts from the current composed frame");
        DispatchMenuCommand(s, app::CmdLayoutSingle);
        frame(now + 45);
        const auto settled = frame(now + 250);
        const bool idle = !renderer.SurfaceAnimating() && !renderer.pane_layout_surface_.HasCurrent();
        check(!settled.empty() && settled != middle && settled == frame(now + 260, false) && idle,
            "collapse reaches the direct single-pane drawing and stops scheduling frames");
        now += 300;
    }

    for (bool high_contrast : {false, true}) {
        DispatchMenuCommand(s, app::CmdLayoutTwoVertical);
        frame(now);
        DispatchMenuCommand(s, app::CmdLayoutSingle);
        const auto pixels = frame(now + 10, high_contrast, high_contrast);
        check(!pixels.empty() && !renderer.pane_layout_surface_.pending &&
            !renderer.pane_layout_surface_.HasCurrent(),
            high_contrast ? "high contrast applies the layout directly" : "disabled motion applies the layout directly");
        now += 100;
    }
    DispatchMenuCommand(s, app::CmdLayoutTwoVertical);
    frame(now);
    s.renameIndex = 0;
    DispatchMenuCommand(s, app::CmdLayoutSingle);
    const auto editing = frame(now + 10);
    check(!editing.empty() && !renderer.pane_layout_surface_.HasCurrent(),
        "an active rename field uses the new layout immediately");
    s.renameIndex = -1;

    renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    return failures ? 1 : 0;
}
