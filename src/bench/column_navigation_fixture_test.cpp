// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution.

#include "../app/app_internal.h"
#include "column_navigation_cases.h"
#include "column_scroll_cases.h"
#include <string_view>

// Link production objects with scripts/test_integration_fixture.ps1.
int main(int argc, char** argv) {
    using namespace pulse;
    ConfigureAnimationTestOutput();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.shot.active = true;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Column navigation fixture",
        WS_POPUP, -30000, -30000, 760, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    const auto output = std::filesystem::absolute(L"../bench_data/issue-review-2026-11-14/columns");
    std::filesystem::create_directories(output);
    const bool passed = argc == 2 && std::string_view(argv[1]) == "--scroll-only"
        ? RunColumnScrollChecks(s, output) : RunColumnNavigationChecks(s, output);
    s.watches.Stop();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return passed ? 0 : 1;
}
