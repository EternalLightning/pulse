// SPDX-License-Identifier: Apache-2.0
#include "toolbar_customization_checks.h"
#include "../common/localization.h"
#include <commctrl.h>
#include <cstdio>

namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    ok ? ++passed : ++failed;
}
constexpr UINT kFixturePointer = WM_APP + 0x2E6;
constexpr UINT_PTR kPointerSubclass = 1;
LRESULT CALLBACK FixturePointerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) return 0;
    if (message == kFixturePointer) {
        if (wparam == WM_NCDESTROY) RemoveWindowSubclass(hwnd, FixturePointerProc, kPointerSubclass);
        else if (wparam) DefSubclassProc(hwnd, static_cast<UINT>(wparam), 0, lparam);
        return TRUE;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}
void CALLBACK InstallFixturePointer(HWND, UINT, UINT_PTR timer, DWORD) {
    // Keep the real cursor's leave notifications from cancelling injected moves.
    const BOOL waiting = EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, ARRAYSIZE(name));
        return wcscmp(name, pulse::ui::kToolbarCustomizationWindowClass) != 0 ||
            !SetWindowSubclass(hwnd, FixturePointerProc, kPointerSubclass, 0);
    }, 0);
    if (!waiting) KillTimer(nullptr, timer);
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
    const UINT_PTR pointer_timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, InstallFixturePointer);
    const auto result = test::RunDialog(spec, true, findings, [&](HWND hwnd, auto& output) {
        ToolbarCustomizationInspection info;
        if (!test::Inspect(hwnd, info)) { output.push_back({false, "inspect scroll fixture"}); return; }
        const auto wait_for = [&](const auto& condition) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            do {
                if (!test::Inspect(hwnd, info)) return false;
                if (condition(info)) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        };
        const auto intermediate = [](float progress) { return progress > 0 && progress < 1; };
        const auto row_progress = [](const auto& rows, int command, bool selection = false) {
            for (const auto& row : rows)
                if (row.command == command) return selection ? row.selection_opacity : row.hover_opacity;
            return -1.0f;
        };
        const bool pointer_fixture_ready = wait_for([&](const auto&) {
            DWORD_PTR ready = 0;
            return test::Send(hwnd, kFixturePointer, 0, 0, &ready) && ready;
        });
        output.push_back({pointer_fixture_ready, "install the isolated pointer driver"});
        const auto hover = [&](D2D1_RECT_F bounds) {
            test::Send(hwnd, kFixturePointer, WM_MOUSEMOVE, test::Center(bounds));
        };
        test::Send(hwnd, kFixturePointer, WM_MOUSELEAVE);
        output.push_back({wait_for([](const auto& state) { return state.available_focus_opacity == 1; }),
            "initial keyboard focus reaches its visible endpoint"});
        const auto available_row = test::Row(info, false, 2);
        const auto current_row = test::Row(info, true, 2);
        hover(available_row);
        output.push_back({wait_for([&](const auto& state) {
            const float progress = row_progress(state.available_rows, 2);
            return state.motion_enabled ? intermediate(progress) : progress == 1;
        }), "available action draws an intermediate hover state"});
        hover(current_row);
        output.push_back({wait_for([&](const auto& state) {
            const float available = row_progress(state.available_rows, 2), current = row_progress(state.current_rows, 2);
            return state.motion_enabled ? intermediate(available) && intermediate(current) : available == 0 && current == 1;
        }), "moving between matching commands keeps independent hover tracks in the two lists"});
        test::Click(hwnd, available_row);
        output.push_back({wait_for([&](const auto& state) {
            const float selection = row_progress(state.available_rows, 2, true);
            return state.available_selection == 2 && (state.motion_enabled
                ? intermediate(selection) && intermediate(state.available_focus_opacity)
                : selection == 1 && state.available_focus_opacity == 0);
        }), "mouse selection updates immediately while row fill and keyboard focus border transition"});
        hover(info.primary);
        output.push_back({wait_for([&](const auto& state) {
            return state.motion_enabled ? intermediate(state.primary_hover_opacity) : state.primary_hover_opacity == 1;
        }), "the Save button draws hover feedback through the shared painter"});
        hover(info.close);
        output.push_back({wait_for([&](const auto& state) {
            return state.motion_enabled ? intermediate(state.close_hover_opacity) : state.close_hover_opacity == 1;
        }), "the title close button draws intermediate hover feedback"});
        test::Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, test::Center(info.close));
        test::Send(hwnd, WM_LBUTTONUP, 0, test::Center(info.available));
        test::Send(hwnd, kFixturePointer, WM_MOUSELEAVE);
        output.push_back({wait_for([](const auto& state) {
            return state.focus == 100 && state.available_focus_opacity == 0 && state.primary_hover_opacity == 0 &&
                state.close_hover_opacity == 0 && !state.interaction_pending;
        }), "a cancelled close press leaves keyboard focus on the list and all fades stop"});
        for (int i = 0; i < 7; ++i) test::Send(hwnd, WM_KEYDOWN, VK_TAB);
        output.push_back({wait_for([](const auto& state) { return state.focus == 100 && state.available_focus_opacity == 1; }),
            "Tab cycles through the dialog controls and restores their keyboard focus cues"});
        test::Send(hwnd, kFixturePointer, WM_NCDESTROY);
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
        wheel(false, -WHEEL_DELTA);
        test::Inspect(hwnd, info);
        const auto thumb = info.available_thumb;
        const auto drag_end = test::Center({thumb.left, info.available.bottom + 2, thumb.right, info.available.bottom + 4});
        test::Send(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, test::Center(thumb));
        test::Send(hwnd, WM_MOUSEMOVE, MK_LBUTTON, drag_end);
        test::Inspect(hwnd, info);
        output.push_back({info.captured && !info.available_scrolling && info.available_scroll > 0 &&
            std::abs(info.available_thumb.bottom - info.available.bottom) < 0.1f && info.current_scroll == 0,
            "thumb drag cancels inertia, reaches the bottom directly and keeps the other list still"});
        const auto drag_start = test::Center({thumb.left, info.available.top - 4, thumb.right, info.available.top - 2});
        test::Send(hwnd, WM_LBUTTONUP, 0, drag_start);
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
    if (pointer_timer) KillTimer(nullptr, pointer_timer);
    for (const auto& [ok, label] : findings) Check(ok, label);
    Check(!result.accepted && result.order.empty(), "close during scrolling cancels the modal without saving preferences");
    OleUninitialize();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
