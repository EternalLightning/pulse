// SPDX-License-Identifier: Apache-2.0
#include "toolbar_customization_checks.h"
#include "../common/localization.h"
#include <cstdio>

namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}
}
int main() {
    using namespace pulse;
    namespace test = bench::toolbar_customization_checks;
    using ui::ToolbarCustomizationInspection;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(L"../bench_data/toolbar-scroll-2026-11-17");
    std::filesystem::create_directories(root);
    bench::RunToolbarCustomizationChecks(root, Check);
    auto spec = test::Spec();
    spec.order.clear();
    for (const auto& action : spec.actions) spec.order.push_back(action.command);
    test::Findings findings;
    const auto result = test::RunDialog(spec, true, findings, [&](HWND hwnd, auto& output) {
        ToolbarCustomizationInspection info;
        if (!test::Inspect(hwnd, info)) { output.push_back({false, "inspect scroll fixture"}); return; }
        const auto wheel = [&](bool current, short delta) {
            const auto rect = current ? info.current : info.available;
            POINT point{static_cast<LONG>((rect.left+rect.right)/2), static_cast<LONG>((rect.top+rect.bottom)/2)};
            ClientToScreen(hwnd, &point);
            return test::Send(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), MAKELPARAM(point.x, point.y));
        };
        wheel(false, -WHEEL_DELTA/2); test::Inspect(hwnd, info);
        output.push_back({info.motion_enabled ? info.available_scrolling && info.available_scroll < 54*info.scale
            : !info.available_scrolling && std::abs(info.available_scroll-54*info.scale) < 0.1f,
            "high precision wheel uses intermediate motion or the system-disabled immediate path"});
        output.push_back({test::WaitScroll(hwnd, false), "available list wheel motion settles"});
        test::Inspect(hwnd, info);
        output.push_back({std::abs(info.available_scroll-54*info.scale) < 0.1f && info.current_scroll == 0,
            "half-notch wheel distance is preserved and the other list stays still"});
        wheel(false, -WHEEL_DELTA);
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        test::Inspect(hwnd, info);
        output.push_back({!info.motion_enabled || (info.available_scrolling && info.available_scroll > 54*info.scale &&
            info.available_scroll < 162*info.scale), "scroll advances through a bounded intermediate position"});
        output.push_back({test::Snapshot(hwnd, root/L"toolbar-scroll-middle.png"), "capture an intermediate scrolling frame"});
        wheel(false, 2*WHEEL_DELTA);
        output.push_back({test::WaitScroll(hwnd, false), "opposite multi-notch wheel input settles"});
        test::Inspect(hwnd, info);
        output.push_back({info.available_scroll < 0.1f, "reverse input cancels old momentum and clamps to the top"});
        test::Click(hwnd, info.available);
        test::Send(hwnd, WM_KEYDOWN, VK_END); test::Inspect(hwnd, info);
        output.push_back({info.available_selection == 24 && (!info.motion_enabled || info.available_scrolling),
            "End updates selection immediately and animates the reveal"});
        output.push_back({test::WaitScroll(hwnd, false), "End reveal reaches its target"});
        test::Inspect(hwnd, info);
        auto last = test::Row(info, false, 24);
        output.push_back({last.right > last.left && last.top >= info.available.top && last.bottom <= info.available.bottom,
            "End reveals the complete final row"});
        test::Send(hwnd, WM_KEYDOWN, VK_HOME); test::Send(hwnd, WM_KEYDOWN, VK_END);
        test::Send(hwnd, WM_KEYDOWN, VK_HOME);
        output.push_back({test::WaitScroll(hwnd, false), "rapid keyboard reversals settle"});
        test::Inspect(hwnd, info);
        output.push_back({info.available_selection == 1 && test::Row(info, false, 1).top >= info.available.top,
            "rapid Home and End preserve the final selected row"});
        for (int i = 0; i < 16; ++i) test::Send(hwnd, WM_KEYDOWN, VK_DOWN);
        output.push_back({test::WaitScroll(hwnd, false), "repeated arrow-key reveal settles"});
        test::Inspect(hwnd, info);
        auto selected = test::Row(info, false, info.available_selection);
        output.push_back({selected.right > selected.left && selected.bottom <= info.available.bottom,
            "arrow navigation reveals its selected row"});
        test::Send(hwnd, WM_KEYDOWN, VK_NEXT); test::Send(hwnd, WM_KEYDOWN, VK_PRIOR);
        output.push_back({test::WaitScroll(hwnd, false), "PageDown and PageUp reveal settles"});
        test::Inspect(hwnd, info);
        const float left_offset = info.available_scroll;
        test::Click(hwnd, info.current); test::Send(hwnd, WM_KEYDOWN, VK_END);
        output.push_back({test::WaitScroll(hwnd, true), "current toolbar list uses the same keyboard scroll model"});
        test::Inspect(hwnd, info); last = test::Row(info, true, 24);
        output.push_back({std::abs(info.available_scroll-left_offset) < 0.1f &&
            last.right > last.left && last.bottom <= info.current.bottom, "the two lists keep independent motion and bounds"});
        output.push_back({test::Snapshot(hwnd, root/L"toolbar-scroll-settled.png"), "capture the settled list bounds and scrollbars"});
        test::Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, test::Center(last));
        test::Send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, test::Center(test::Row(info, true, 23)));
        wheel(true, WHEEL_DELTA);
        test::Send(hwnd, WM_KEYDOWN, VK_ESCAPE); test::Inspect(hwnd, info);
        output.push_back({!info.captured && !info.dragging_command && !info.current_scrolling,
            "cancelling a drag also stops its pending scroll instead of moving the released list"});
        wheel(true, WHEEL_DELTA); test::Click(hwnd, info.current); test::Inspect(hwnd, info);
        output.push_back({!info.current_scrolling && !info.available_scrolling, "direct selection cancels conflicting inertia"});
        wheel(false, -WHEEL_DELTA);
        test::Send(hwnd, WM_SIZE, 0, MAKELPARAM(780, 500)); test::Inspect(hwnd, info);
        output.push_back({!info.current_scrolling && !info.available_scrolling && info.current_scroll >= 0,
            "viewport resize cancels momentum and revalidates both offsets"});
        wheel(true, WHEEL_DELTA);
    });
    for (const auto& [ok, label] : findings) Check(ok, label);
    Check(!result.accepted && result.order.empty(), "close during scrolling cancels the modal without saving preferences");
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
