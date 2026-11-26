// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../app/tab_controller.h"
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    using namespace pulse;
    using namespace pulse::app;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = state.ctxMenuPrefs.persist = state.searchHistory.persist = false;
    state.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"tab fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 1400, 800, nullptr, nullptr, nullptr, nullptr);
    if (!state.hwnd || !state.compositor.Init(state.hwnd)) return 2;
    state.renderer.SetCompositor(&state.compositor);
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const auto output = std::filesystem::absolute(argc > 1 ? argv[1] : "../bench_data/tab-close-interaction");
    std::filesystem::create_directories(output);
    AppPrefs prefs;
    prefs.persist = false;
    prefs.launch_on_startup = prefs.close_last_tab_window = true;
    AppPrefs loaded;
    loaded.persist = false;
    check(loaded.FromJson(prefs.ToJson()) && loaded.launch_on_startup && loaded.close_last_tab_window,
        "sign-in startup and last-tab options round trip without real preferences");
    check(loaded.FromJson(L"{}") && !loaded.launch_on_startup && !loaded.close_last_tab_window,
        "legacy preferences leave sign-in startup and last-tab closing off");
    bool close_last = false;
    int close_requests = 0, layout_changes = 0;
    TabController::Callbacks callbacks;
    callbacks.close_window_with_last_tab = [&] { return close_last; };
    callbacks.close_window = [&] { ++close_requests; };
    callbacks.layout_changed = [&] { ++layout_changes; };
    TabController controller(std::move(callbacks));
    WindowTabs tabs;
    tabs.EnsureDefault();
    check(!controller.CanCloseTab(tabs, 0), "last tab cannot close from its menu with the option off");
    controller.CloseTab(tabs, 0);
    check(tabs.items.size() == 1 && close_requests == 0 && layout_changes == 0,
        "disabled last-tab close preserves the model and window");
    close_last = true;
    check(controller.CanCloseTab(tabs, 0), "last-tab menu accepts the close-window option");
    controller.CloseTab(tabs, 0);
    check(close_requests == 1 && tabs.items.size() == 1,
        "last-tab close delegates to the normal window-close policy without deleting the last tab");
    tabs.items[0]->pinned = true;
    controller.CloseTab(tabs, 0);
    check(!controller.CanCloseTab(tabs, 0) && close_requests == 1 && tabs.items.size() == 1,
        "pinned last tab never closes the window");
    tabs.NewTab(L"C:\\fixture");
    controller.CloseTab(tabs, 1);
    check(tabs.items.size() == 1 && tabs.items[0]->pinned && close_requests == 1,
        "ordinary menu close only removes its unpinned tab");
    check(!controller.CanCloseTab(tabs, 999), "invalid tab menu indices cannot close anything");

    TrayController tray;
    check(!tray.StartHidden(true) && !tray.SetVisible(true),
        "missing window never hides startup behind a missing tray icon");
    tray.HandleTaskbarCreated();
    check(!tray.IsVisible() && TrayController::TaskbarCreatedMessage() != 0,
        "taskbar restart handler is safe without an attached window");
    auto close_point = [&](int target, POINT& point) {
        const auto model = BuildVm(state, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
            static_cast<float>(state.compositor.Height()));
        const int y = static_cast<int>(state.renderer.TitleBarHeight() / 2);
        for (int x = 0; x < static_cast<int>(bounds.right); ++x) {
            const auto hit = state.renderer.HitTest(model, bounds, static_cast<float>(x), static_cast<float>(y));
            if (hit.region == ui::HitTestResult::TabClose && hit.index == target) {
                point = {x, y};
                return true;
            }
        }
        return false;
    };
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        state.scale = scale;
        state.renderer.SetScale(scale);
        state.compositor.RecreateTextFormats(scale);
        state.compositor.Resize(static_cast<UINT>(1400 * scale), static_cast<UINT>(800 * scale));
        state.window_tabs = app::WindowTabs{};
        state.window_tabs.EnsureDefault();
        state.window_tabs.items[0]->pinned = true;
        for (int i = 0; i < 4; ++i) state.window_tabs.NewTab(app::MakeHomePath());
        state.pane = state.window_tabs.Active()->FocusedPane();
        {
            const auto model = BuildVm(state, false);
            const auto bounds = D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                static_cast<float>(state.compositor.Height()));
            D2D1_RECT_F tab{};
            const bool found = state.renderer.TabItemRect(model, bounds.right, 4, &tab);
            const float button_left = tab.right - 34.0f * scale;
            const float button_top = (tab.top + tab.bottom - 28.0f * scale) * 0.5f;
            bool clickable = found;
            for (const float x : {button_left + scale, tab.right - 7.0f * scale})
                for (const float y : {button_top + scale, button_top + 27.0f * scale}) {
                    const auto hit = state.renderer.HitTest(model, bounds, x, y);
                    clickable &= hit.region == ui::HitTestResult::TabClose && hit.index == 4;
                }
            check(clickable, "the enlarged 28 DIP close button is clickable across its whole visual area");
            check(state.renderer.HitTest(model, bounds, button_left - scale,
                (tab.top + tab.bottom) * 0.5f).region == ui::HitTestResult::Tab,
                "the title beside the enlarged button still switches the tab");
        }
        for (const bool dark : {false, true}) for (const int width : {480, 1400}) {
            state.compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(800 * scale));
            state.darkMode = dark;
            auto model = BuildVm(state, false);
            model.hover_region = ui::HitTestResult::TabClose;
            model.hover_control_index = 4;
            const auto bounds = D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                static_cast<float>(state.compositor.Height()));
            state.compositor.Dc()->BeginDraw();
            state.renderer.Render(model, bounds, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            check(SUCCEEDED(state.compositor.Dc()->EndDraw()), "tab close button renders in both themes");
            const auto name = std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png");
            check(state.compositor.SaveSnapshot((output / name).c_str()), "save tab close button visual evidence");
        }
        for (int click = 0; click < 4; ++click) {
            POINT point{};
            const int target = static_cast<int>(state.window_tabs.items.size()) - 1;
            const bool found = close_point(target, point);
            check(found, "remaining tab close button is hit-testable");
            if (!found) break;
            const auto before = state.window_tabs.items.size();
            const LPARAM position = MAKELPARAM(point.x, point.y);
            if (click % 2) {
                state.stripClickTick = GetTickCount64();
                HandleLButtonDblClk(&state, state.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
            } else {
                HandleLButtonDown(&state, state.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
            }
            check(state.window_tabs.items.size() + 1 == before,
                "every rapid press closes one tab without waiting, including DBLCLK");
            HandleLButtonUp(&state, state.hwnd, WM_LBUTTONUP, 0, position);
        }
        POINT point{};
        check(state.window_tabs.items.size() == 1 && !close_point(0, point),
            "rapid closing preserves pinned tabs and their missing close button");
        state.window_tabs.items[0]->pinned = false;
        state.appPrefs.close_last_tab_window = false;
        check(close_point(0, point), "ordinary last tab retains its existing close affordance");
        HandleLButtonDblClk(&state, state.hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        check(state.window_tabs.items.size() == 1, "disabled last-tab window close preserves the final tab");
    }
    DestroyWindow(state.hwnd);
    state.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
