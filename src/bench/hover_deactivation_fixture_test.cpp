// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/app_hover.h"
#include "../app/app_ui_schedule.h"
#include "../common/localization.h"
#include <cstdio>

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace {
int failures = 0;
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
void SeedHover(pulse::AppState& state) {
    state.mouseLeaveTracked = true;
    state.scrollbarHovered = true;
    state.hoverRow = state.hoverPaneIndex = state.breadcrumbHover = 1;
    state.hoverRegion = pulse::ui::HitTestResult::Row;
    state.hoverControlIndex = state.hoverSubIndex = 1;
    state.hoverPath = L"C:\\fixture\\file.txt";
    state.hoverLabel = L"file.txt";
    state.tooltipText = L"file.txt - file details";
    state.hoverSince = state.ctxHoverSince = GetTickCount64() - 1000;
    state.ui_schedule.tooltip_checked_since = 0;
    state.dragPending = true;
}
bool HoverCleared(const pulse::AppState& state) {
    return !state.mouseLeaveTracked && !state.scrollbarHovered &&
        state.hoverRow == -1 && state.hoverPaneIndex == -1 && state.breadcrumbHover == -1 &&
        !state.hoverRegion && state.hoverControlIndex == -1 && state.hoverSubIndex == -1 &&
        state.hoverPath.empty() && state.hoverLabel.empty() && state.tooltipText.empty() &&
        !state.hoverSince && !state.ctxHoverSince;
}
}

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = state.searchHistory.persist = state.ctxMenuPrefs.persist = false;
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Hover deactivation fixture",
        WS_POPUP | WS_VISIBLE, -30000, -30000, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(state.hwnd != nullptr, "create offscreen window without activating it");
    if (!state.hwnd) { owned.reset(); OleUninitialize(); return 2; }
    SetWindowLongPtrW(state.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    const HWND foreground = GetForegroundWindow();
    for (const UINT message : {WM_ACTIVATE, WM_ACTIVATEAPP}) {
        SeedHover(state);
        WndProcImpl(state.hwnd, message, 0, 0);
        Check(HoverCleared(state) && !state.dragPending,
            "window and application deactivation clear hover identity, tooltip and delayed hover clocks");
        RunUiDeadlines(state, GetTickCount64() + 1000);
        ui::WindowViewModel vm;
        RefreshStationaryHover(state, vm);
        Check(HoverCleared(state), "timer and stationary-pointer refresh cannot revive an inactive tooltip");
    }
    SeedHover(state);
    SetCapture(state.hwnd);
    Check(GetCapture() == state.hwnd, "isolated window acquires capture for drag regression");
    WndProcImpl(state.hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
    Check(HoverCleared(state) && state.dragPending && GetCapture() == state.hwnd,
        "window deactivation clears tooltip while preserving a captured drag");
    ReleaseCapture();
    state.dragPending = false;
    ui::HitTestResult hit;
    hit.region = ui::HitTestResult::AddressSearch;
    UpdateHoverFromHit(state, hit, POINT{20, 20});
    const auto hover_since = state.hoverSince;
    RunUiDeadlines(state, hover_since + 151);
    Check(!state.tooltipText.empty() && state.ui_schedule.tooltip_checked_since == hover_since,
        "a new hover can show its tooltip after the normal delay");
    Check(GetForegroundWindow() == foreground, "fixture never changes the foreground window");
    SetWindowLongPtrW(state.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(state.hwnd);
    state.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
