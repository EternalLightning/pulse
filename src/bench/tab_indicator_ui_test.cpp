// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/animation_clock.h"
#include "../app/context_menu.h"
#include "snapshot_output.h"
#include <cmath>
#include <cstdio>
#include <filesystem>

LRESULT CALLBACK WndProcImpl(HWND, UINT, WPARAM, LPARAM);

namespace pulse::ui {
struct FluentMenuTestPeer {
    inline static FluentMenu* menu = nullptr;
    inline static std::vector<int> commands;
    static void CALLBACK Choose(HWND, UINT, UINT_PTR, DWORD) {
        if (!menu || !menu->IsOpen() || menu->animating_out_ || commands.empty()) return;
        const int command = commands.front();
        commands.erase(commands.begin());
        if (command) menu->InvokeCommand(command);
        else menu->RequestDismiss();
    }
};

struct TabIndicatorUiTest {
    inline static int failures = 0;
    inline static int timers = 0;
    inline static int paints = 0;
    inline static bool keep_animating = false;
    static void Check(bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        failures += !ok;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_TIMER && wp == 0x1974) { ++timers; return 0; }
        if (msg == WM_PAINT && s) {
            ++paints;
            if (keep_animating) {
                s->window_tabs.SwitchTab(1 - s->window_tabs.active);
                s->pane = s->window_tabs.Active()->FocusedPane();
            }
            return WndProcImpl(hwnd, msg, wp, lp);
        }
        if (msg == WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static void Settle(AppState& s) {
        auto vm = BuildVm(s, false);
        D2D1_RECT_F tab{};
        s.renderer.TabItemRect(vm, static_cast<float>(s.compositor.Width()),
            static_cast<int>(s.window_tabs.active), &tab);
        auto& line = s.renderer.tab_indicator_;
        line.Tick(line.identity, tab.left + 16 * s.scale, tab.right - 16 * s.scale,
            AnimationTimeMs() + 1000, true);
    }
    static void StripMenuChecks(AppState& s, const std::filesystem::path& output) {
        FluentMenu menu;
        if (!menu.Create(s.hwnd, &s.compositor, 1)) {
            Check(false, "initialize production tab context menu");
            return;
        }
        int captures = 0;
        std::function<void()> mutation_probe;
        app::TabController::Callbacks callbacks;
        callbacks.invalidate = [&] {
            s.hitTestVmValid = false;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            if (mutation_probe) mutation_probe();
        };
        callbacks.layout_changed = [&] { BindCurrentLayout(s); };
        callbacks.will_change_tab_strip = [&] { ++captures; CaptureTabStripLayout(s); };
        s.tabs.SetCallbacks(std::move(callbacks));
        const int commands[] = {app::CmdTabClose, app::CmdTabCloseOthers, app::CmdTabCloseRight,
            app::CmdTabGroupClose, app::CmdTabAddToNewGroup, app::CmdTabJoinGroupBase + 1,
            app::CmdTabRemoveFromGroup, app::CmdTabGroupUngroup, app::CmdTabPin, 0, 0};
        const char* labels[] = {"context close", "context close others", "context close right",
            "context close group", "context create group", "context join group",
            "context remove from group", "context ungroup", "context pin", "collapse group", "expand group"};
        for (size_t scenario = 0; scenario < std::size(commands); ++scenario) {
            s.renderer.tab_close_.Reset();
            s.window_tabs = app::WindowTabs{};
            for (int i = 0; i < 4; ++i) s.window_tabs.NewTab(app::MakeHomePath());
            s.window_tabs.tab_groups = {{7, L"First", 0x0080FF, scenario == 10}, {8, L"Second", 0xFF8000, false}};
            s.window_tabs.items[0]->tab_group = s.window_tabs.items[1]->tab_group = 7;
            s.window_tabs.items[3]->tab_group = 8;
            BindCurrentLayout(s);
            Render(s); Settle(s); Render(s);
            const auto before = s.renderer.ComputeTabStrip(BuildVm(s, false), 1100);
            const int previous_captures = captures;
            MainRenderer::TabStripMetrics creation_frame;
            bool creation_sampled = false, creation_animating = false;
            mutation_probe = {};
            if (scenario == 4) {
                // The rename palette runs while the new group is already animating.
                mutation_probe = [&] {
                    if (creation_sampled) return;
                    creation_sampled = true;
                    creation_frame = s.renderer.ComputeTabStrip(BuildVm(s, false), 1100);
                    creation_animating = s.renderer.TabCloseAnimating();
                    Sleep(90); Render(s);
                    Check(bench::SaveSnapshot(s.compositor, output / L"group-create-middle.png"),
                        "save production group-transition intermediate frame");
                };
            }
            if (scenario >= 9) {
                s.tabs.ToggleGroupCollapse(s.window_tabs, 7);
            } else {
                FluentMenuTestPeer::menu = &menu;
                FluentMenuTestPeer::commands = {commands[scenario]};
                if (scenario == 4) FluentMenuTestPeer::commands.push_back(0);
                const auto timer = SetTimer(nullptr, 0, 15, FluentMenuTestPeer::Choose);
                if (!timer) { Check(false, "start deterministic context-menu selection timer"); return; }
                if (scenario == 3 || scenario == 7)
                    s.tabs.ShowGroupMenu(s.window_tabs, 7, POINT{80, 80}, menu);
                else
                    s.tabs.ShowTabMenu(s.window_tabs, scenario == 0 ? 0 : scenario == 2 ? 1 : scenario == 6 ? 3 : 2,
                        POINT{80, 80}, menu);
                KillTimer(nullptr, timer);
                FluentMenuTestPeer::menu = nullptr;
                Check(FluentMenuTestPeer::commands.empty(), "requested context-menu command was dispatched");
            }
            std::printf("[SCENARIO] %s\n", labels[scenario]);
            Check(captures > previous_captures, "tab-strip mutation captures the displayed geometry before changing the model");
            mutation_probe = {};
            const auto after = scenario == 4 ? creation_frame : s.renderer.ComputeTabStrip(BuildVm(s, false), 1100);
            Check(scenario == 4 ? creation_sampled && creation_animating : s.renderer.TabCloseAnimating(),
                "tab-strip mutation schedules the existing reflow animation");
            bool continuous = true;
            for (const auto& item : after.items) {
                const auto old = std::find_if(before.items.begin(), before.items.end(),
                    [&](const auto& candidate) { return candidate.SameKey(item); });
                if (old != before.items.end())
                    continuous &= std::abs(item.left - old->left) < 3 && std::abs(item.right - old->right) < 3;
            }
            Check(continuous, "surviving tab, chip and plus-button spans begin at their displayed positions");
            Sleep(90); Render(s);
            if (scenario == 7) {
                Check(bench::SaveSnapshot(s.compositor, output / L"group-ungroup-middle.png"),
                    "save production group-transition intermediate frame");
            }
            Sleep(140); Render(s);
            Check(!s.renderer.TabCloseAnimating(), "tab-strip mutation settles and releases frame scheduling");
        }
        s.renderer.motion_enabled_ = false;
        s.tabs.ToggleGroupCollapse(s.window_tabs, 7);
        Render(s);
        Check(!s.renderer.TabCloseAnimating(), "group mutations respect disabled motion");
        s.renderer.motion_enabled_ = true;
        s.tabs.SetCallbacks({});
        s.window_tabs.tab_groups.clear();
        for (auto& tab : s.window_tabs.items) tab->tab_group = 0;
    }
    static int Run() {
        const auto output = std::filesystem::absolute(L"bench_data/tab-indicator-followup");
        std::filesystem::create_directories(output);
        auto owned = std::make_unique<AppState>();
        auto& s = *owned;
        s.isolatedTest = s.shot.active = true;
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = false;
        s.appPrefs.new_tab_home = true;
        s.accentColor = HexColor(0x0078D4);
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"PulseTabIndicatorFollowup";
        cls.lpfnWndProc = Proc;
        RegisterClassW(&cls);
        s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, cls.lpszClassName, L"Tab animation fixture",
            WS_POPUP, 50, 50, 960, 640, nullptr, nullptr, cls.hInstance, nullptr);
        if (!s.hwnd || !s.compositor.Init(s.hwnd)) { Check(false, "initialize production fixture"); return 1; }
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetScale(1);
        s.renderer.SetDetailsPanelVisible(false);
        s.renderer.motion_enabled_ = true;
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
        s.window_tabs.NewTab(app::MakeHomePath());
        s.pane = s.window_tabs.Active()->FocusedPane();
        Render(s);
        auto& line = s.renderer.tab_indicator_;
        const float before_left = line.left, before_right = line.right;
        auto vm = BuildVm(s, false);
        const auto button = s.renderer.ComputeTabStrip(vm, 960).new_button;
        const auto click = MAKELPARAM(static_cast<int>((button.left + button.right) * 0.5f), 24);
        WndProcImpl(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, click);
        WndProcImpl(s.hwnd, WM_LBUTTONUP, 0, click);
        Check(s.window_tabs.items.size() == 2 && s.window_tabs.active == 1,
            "production plus-button pointer messages create and select a tab");
        Render(s);
        Check(line.pending && line.left == before_left && line.right == before_right,
            "new tab animates from the existing underline instead of snapping after count changes");
        Sleep(90);
        Render(s);
        Check(line.pending && line.left > before_left && line.right > before_right,
            "both edges advance during the real new-tab transition");
        Check(bench::SaveSnapshot(s.compositor, output / L"new-tab-middle.png"), "save new-tab intermediate frame");
        Settle(s);
        Render(s);
        Check(!line.pending, "new-tab transition releases frame scheduling at rest");
        for (int i = 0; i < 12; ++i) {
            const float previous_left = line.left, previous_right = line.right;
            s.window_tabs.NewTab(app::MakeHomePath());
            s.pane = s.window_tabs.Active()->FocusedPane();
            Render(s);
            auto next_vm = BuildVm(s, false);
            D2D1_RECT_F next_tab{};
            s.renderer.TabItemRect(next_vm, 960, static_cast<int>(s.window_tabs.active), &next_tab);
            const bool same_position = std::abs(previous_left - (next_tab.left + 16)) < 0.001f &&
                std::abs(previous_right - (next_tab.right - 16)) < 0.001f;
            if (same_position) {
                Check(!line.pending && std::abs(line.left - previous_left) < 0.001f &&
                    std::abs(line.right - previous_right) < 0.001f,
                    "overflow revealing a new tab at the same visible span avoids a pointless frame loop");
            } else {
                Check(line.pending && line.left == previous_left && line.right == previous_right,
                    "new tab retains its previous visible span while tabs shrink or enter overflow");
            }
            Settle(s);
            Render(s);
        }
        s.window_tabs.SwitchTab(0);
        s.pane = s.window_tabs.Active()->FocusedPane();
        Render(s);
        Check(line.pending, "switching across a scrolling tab strip keeps the transition");
        s.renderer.motion_enabled_ = false;
        Render(s);
        Check(!line.pending, "disabling motion settles a running transition");
        s.renderer.motion_enabled_ = true;
        s.window_tabs.SwitchTab(1);
        s.pane = s.window_tabs.Active()->FocusedPane();
        Render(s);
        s.compositor.Resize(1100, 640);
        SetWindowPos(s.hwnd, nullptr, 0, 0, 1100, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Render(s);
        Check(!line.pending, "window resize snaps stale coordinates without chasing layout");
        s.renderer.SetScale(1.5f);
        Render(s);
        Check(!line.pending, "DPI changes settle directly on current geometry");
        s.renderer.SetScale(1);
        s.window_tabs.items.clear();
        for (int i = 0; i < 4; ++i) s.window_tabs.NewTab(app::MakeHomePath());
        s.pane = s.window_tabs.Active()->FocusedPane();
        Render(s); Settle(s); Render(s);
        auto four_vm = BuildVm(s, false);
        const auto four_strip = s.renderer.ComputeTabStrip(four_vm, 1100);
        const auto active_identity = s.window_tabs.Active();
        D2D1_RECT_F first{};
        s.renderer.TabItemRect(four_vm, 1100, 0, &first);
        const auto close_click = MAKELPARAM(static_cast<int>(first.right - 18), 24);
        WndProcImpl(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, close_click);
        WndProcImpl(s.hwnd, WM_LBUTTONUP, 0, close_click);
        Check(s.window_tabs.items.size() == 3 && s.window_tabs.Active() == active_identity,
            "production pointer close removes the leftmost tab while retaining the active layout");
        WndProcImpl(s.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(300, 24));
        Check(s.hitTestVm.tabs.size() == 3 && s.renderer.TabCloseAnimating(),
            "pointer hit testing before the first paint cannot consume the close using stale cached tabs");
        Render(s);
        auto close_vm = BuildVm(s, false);
        auto close_strip = s.renderer.ComputeTabStrip(close_vm, 1100);
        Check(s.renderer.TabCloseAnimating() &&
            std::abs(close_strip.items[0].left - four_strip.items[1].left) < 3 &&
            std::abs(close_strip.new_button.left - four_strip.new_button.left) < 3,
            "close starts at displayed tab and plus-button positions");
        Sleep(85);
        D2D1_RECT_F frame_before{}, frame_after{};
        s.renderer.TabItemRect(close_vm, 1100, 2, &frame_before);
        const auto render_start = AnimationTimeMs();
        Render(s);
        const auto render_end = AnimationTimeMs();
        s.renderer.TabItemRect(close_vm, 1100, 2, &frame_after);
        close_strip = s.renderer.ComputeTabStrip(close_vm, 1100);
        const float middle_left = close_strip.items[0].left;
        Check(middle_left > 12 && middle_left < four_strip.items[1].left,
            "remaining tabs visibly move into the removed slot");
        D2D1_RECT_F moving{};
        s.renderer.TabItemRect(close_vm, 1100, 0, &moving);
        auto hit = s.renderer.HitTest(close_vm, D2D1::RectF(0, 0, 1100, 640),
            (moving.left + moving.right) * 0.5f, 24);
        Check(hit.region == HitTestResult::Tab && hit.index == 0,
            "hit testing follows the moving tab instead of the final slot");
        D2D1_RECT_F active_rect{};
        s.renderer.TabItemRect(close_vm, 1100, 2, &active_rect);
        std::printf("[FRAME] close render=%llu..%llu later=%llu line=%.3f before=%.3f after=%.3f later=%.3f pending=%d\n",
            static_cast<unsigned long long>(render_start), static_cast<unsigned long long>(render_end),
            static_cast<unsigned long long>(AnimationTimeMs()), line.left, frame_before.left + 16,
            frame_after.left + 16, active_rect.left + 16, line.pending);
        // The rendered line and a later hit-test layout use different clock samples.
        Check(!line.pending &&
            line.left >= std::min(frame_before.left, frame_after.left) + 16 - 0.01f &&
            line.left <= std::max(frame_before.left, frame_after.left) + 16 + 0.01f &&
            line.right >= std::min(frame_before.right, frame_after.right) - 16 - 0.01f &&
            line.right <= std::max(frame_before.right, frame_after.right) - 16 + 0.01f,
            "active underline follows the closing reflow without a second chase animation");
        Check(bench::SaveSnapshot(s.compositor, output / L"close-left-middle.png"),
            "save production close intermediate frame");
        CloseLayoutTab(s, 0); Render(s);
        Check(s.renderer.TabCloseAnimating() && s.window_tabs.items.size() == 2,
            "a consecutive production close retargets the current reflow");
        Sleep(230); Render(s);
        Check(!s.renderer.TabCloseAnimating(), "closing releases the production frame loop at rest");
        CloseActiveTab(s); Render(s);
        Check(s.window_tabs.items.size() == 1 && s.renderer.TabCloseAnimating(),
            "closing the active tab also animates the remaining strip");
        s.renderer.motion_enabled_ = false; Render(s);
        Check(!s.renderer.TabCloseAnimating() && !line.pending,
            "disabling motion settles closing and underline together");
        s.renderer.motion_enabled_ = true;
        s.window_tabs.items.clear();
        for (int i = 0; i < 4; ++i) s.window_tabs.NewTab(app::MakeHomePath());
        s.window_tabs.items[0]->pinned = true;
        s.window_tabs.tab_groups = {{7, L"First", 0x0080FF, false}, {8, L"Second", 0xFF8000, false}};
        s.window_tabs.items[1]->tab_group = s.window_tabs.items[2]->tab_group = 7;
        s.window_tabs.items[3]->tab_group = 8;
        s.pane = s.window_tabs.Active()->FocusedPane(); Render(s); Settle(s); Render(s);
        const auto grouped_strip = s.renderer.ComputeTabStrip(BuildVm(s, false), 1100);
        CloseLayoutTab(s, 1); Render(s);
        auto grouped_close = s.renderer.ComputeTabStrip(BuildVm(s, false), 1100);
        Check(s.renderer.TabCloseAnimating() && grouped_close.items[0] == grouped_strip.items[0],
            "a pinned tab retains its width and position while grouped members close");
        Check(std::abs(grouped_close.chips[1].left - grouped_strip.chips[1].left) < 3,
            "later group chips start at their displayed positions");
        CloseLayoutTab(s, 1); Render(s); Sleep(230); Render(s);
        Check(!s.renderer.TabCloseAnimating() &&
            s.renderer.ComputeTabStrip(BuildVm(s, false), 1100).chips.size() == 1,
            "closing the last group member removes its chip and settles the surviving group");
        s.window_tabs.tab_groups.clear();
        for (auto& tab : s.window_tabs.items) { tab->pinned = false; tab->tab_group = -1; }
        for (int i = 0; i < 12; ++i) s.window_tabs.NewTab(app::MakeHomePath());
        s.pane = s.window_tabs.Active()->FocusedPane(); Render(s); Settle(s); Render(s);
        CloseLayoutTab(s, 0); Render(s);
        close_vm = BuildVm(s, false);
        s.renderer.TabItemRect(close_vm, 1100, static_cast<int>(s.window_tabs.active), &active_rect);
        Check(std::isfinite(active_rect.left) && std::isfinite(active_rect.right),
            "overflow close keeps finite, reachable active-tab geometry");
        SetWindowPos(s.hwnd, nullptr, 0, 0, 1080, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(1080, 640); Render(s);
        Check(!s.renderer.TabCloseAnimating(), "resizing cancels closing coordinates from the old window width");
        s.window_tabs.items.resize(2);
        s.window_tabs.SwitchTab(0);
        s.pane = s.window_tabs.Active()->FocusedPane();
        Render(s); Settle(s); Render(s);
        s.shot.active = false;
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
        timers = paints = 0;
        keep_animating = true;
        SetTimer(s.hwnd, 0x1974, 40, nullptr);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(650)) {
            MSG msg{};
            if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { Sleep(1); continue; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        keep_animating = false;
        KillTimer(s.hwnd, 0x1974);
        Check(paints >= 3 && timers >= 3, "continuous production WM_PAINT delivers due timers without starving them");
        std::printf("[PERF] continuous tab transitions: paints=%d timers=%d\n", paints, timers);
        Settle(s);
        for (int frame = 0; frame < 30 && Render(s); ++frame) Sleep(20);
        Check(!Render(s), "earlier page transitions settle before the isolated toast check");
        s.notification_toast.SetMotionEnabled(true);
        s.notification_toast.Show(s.hwnd, L"Animation", L"Display-paced toast", false);
        ValidateRect(s.hwnd, nullptr);
        s.notification_toast.HandleMessage(s.hwnd, WM_TIMER, 0x50554C54, 0);
        Check(!GetUpdateRect(s.hwnd, nullptr, FALSE),
            "toast lifetime timer cannot duplicate the owner's reveal frame scheduling");
        Check(Render(s) && s.notification_toast.Animating(), "toast entrance joins display-paced frame scheduling");
        Sleep(180);
        const bool toast_frame = Render(s);
        if (toast_frame || s.notification_toast.Animating())
            std::printf("[TOAST] toast=%d tab=%d close=%d surface=%d settings=%d\n",
                s.notification_toast.Animating(), s.renderer.TabIndicatorAnimating(), s.renderer.TabCloseAnimating(),
                s.renderer.SurfaceAnimating(), s.renderer.SettingsSwitchAnimating());
        Check(!toast_frame && !s.notification_toast.Animating(), "toast settles without a permanent render loop");
        s.notification_toast.HandleMessage(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        ShowWindow(s.hwnd, SW_HIDE);
        ValidateRect(s.hwnd, nullptr);
        WndProcImpl(s.hwnd, WM_PAINT, 0, 0);
        Check(!GetUpdateRect(s.hwnd, nullptr, FALSE), "hidden window does not queue animation frames");
        s.shot.active = true;
        SetWindowPos(s.hwnd, nullptr, 0, 0, 1100, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        s.compositor.Resize(1100, 640);
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        StripMenuChecks(s, output);
        ShowWindow(s.hwnd, SW_HIDE);
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        s.renderer.SetCompositor(nullptr);
        return failures ? 1 : 0;
    }
};
} // namespace pulse::ui

int RunTabIndicatorUiTest() { return pulse::ui::TabIndicatorUiTest::Run(); }
