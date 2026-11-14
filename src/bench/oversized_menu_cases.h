// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution.

struct OversizedMenuInput {
    pulse::ui::FluentMenu* menu = nullptr;
    bool opening_release = false;
    bool survived_release = false;
    int phase = 0;
    ULONGLONG started = 0;
};
OversizedMenuInput* oversized_input = nullptr;

void CALLBACK DriveOversizedMenu(HWND, UINT, UINT_PTR timer, DWORD) {
    using pulse::ui::FluentMenuTestPeer;
    auto& input = *oversized_input;
    const HWND hwnd = FluentMenuTestPeer::Window(*input.menu);
    const auto point = FluentMenuTestPeer::FirstRowPoint(*input.menu);
    if (input.phase == 0 && input.opening_release) {
        PostMessageW(hwnd, WM_LBUTTONUP, 0, point);
        ++input.phase;
    } else if (input.phase <= 1) {
        input.survived_release = input.menu->IsOpen();
        PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        PostMessageW(hwnd, WM_LBUTTONUP, 0, point);
        input.phase = 2;
    } else if (GetTickCount64() - input.started > 2000) {
        PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        KillTimer(nullptr, timer);
    }
}

void RunOversizedMenus(HWND owner, pulse::ui::Compositor& compositor,
                       const std::filesystem::path& output) {
    using namespace pulse::ui;
    std::filesystem::create_directories(output);
    POINT origin{};
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const POINT anchor{work.left + 80, work.top + (work.bottom - work.top) / 3};
    const HWND foreground = GetForegroundWindow();
    for (const float scale : {1.0f, 1.5f, 2.25f}) for (const bool dark : {false, true}) {
        FluentMenu menu;
        Check(menu.Create(owner, &compositor, scale), "create oversized-menu fixture");
        menu.SetTheme(dark, D2D1::ColorF(0x0078D4));
        std::vector<FluentMenuItem> items;
        const int count = static_cast<int>((work.bottom - work.top) / (30 * scale)) + 20;
        for (int i = 0; i < count; ++i) {
            FluentMenuItem item;
            item.command = i + 1;
            item.text = L"More action / 更多操作 " + std::to_wstring(i + 1);
            items.push_back(std::move(item));
        }
        const auto bounds = FluentMenuTestPeer::LayoutAt(menu, anchor, items);
        Check(bounds.top == anchor.y && bounds.bottom <= work.bottom,
            "oversized command menu stays below its anchor and within the work area");
        Check(FluentMenuTestPeer::Scrolls(menu), "oversized command menu scrolls instead of covering its opener");
        const auto name = std::to_wstring(static_cast<int>(scale * 100)) +
            (dark ? L"-dark.png" : L"-light.png");
        Check(menu.SaveDebugSnapshot((output / name).c_str(), items), "save constrained menu at tested theme and DPI");
        Check(FluentMenuTestPeer::RevealLast(menu), "height-constrained menu can reveal its last command");
        const auto bottom = FluentMenuTestPeer::LayoutAt(menu, {anchor.x, work.bottom - 5}, items);
        Check(bottom.top >= work.top && bottom.bottom <= work.bottom,
            "an anchor without one row below still falls back inside the work area");
    }
    BYTE saved[256]{};
    Check(GetKeyboardState(saved) != FALSE, "save only this test thread's keyboard state");
    for (const bool opening_release : {true, false}) {
        BYTE keys[256];
        std::copy(std::begin(saved), std::end(saved), std::begin(keys));
        keys[VK_LBUTTON] = opening_release ? 0x80 : 0;
        SetKeyboardState(keys);
        FluentMenu menu;
        Check(menu.Create(owner, &compositor, 1), "create isolated menu input fixture");
        FluentMenuItem item;
        item.command = 777;
        item.text = L"Intentional action";
        OversizedMenuInput input{&menu, opening_release, false, 0, GetTickCount64()};
        oversized_input = &input;
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, DriveOversizedMenu);
        Check(timer != 0, "start bounded synthetic menu input");
        if (timer) {
            const int command = menu.TrackPopup(anchor, {item});
            KillTimer(nullptr, timer);
            Check(command == 777 && (!opening_release || input.survived_release), opening_release
                ? "opening release cannot execute a row; the next complete click can"
                : "a keyboard-opened menu accepts its first complete click");
        }
        SetKeyboardState(saved);
    }
    oversized_input = nullptr;
    Check(GetForegroundWindow() == foreground, "menu checks preserve the user's foreground window");
}
