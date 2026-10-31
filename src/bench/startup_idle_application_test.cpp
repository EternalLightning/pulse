// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/app_ui_schedule.h"
#include "../app/ui_change_signal.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <thread>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
template<class Ready> bool Wait(Ready ready, bool pump = true) {
    const auto until = GetTickCount64() + 5000;
    while (!ready() && GetTickCount64() < until) {
        if (pump) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        Sleep(5);
    }
    return ready();
}
void Configure(pulse::AppState& s, const std::filesystem::path& directory) {
    s.isolatedTest = true;
    s.safeMode = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.core.shared_preferences_loaded = true;
    s.open_path = directory.wstring();
}
}

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"startup-idle-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root / L"first");
    std::filesystem::create_directories(root / L"second");
    std::ofstream(root / L"second" / L"sample.txt") << "early preview";
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"PulseStartupIdleFixture";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const auto create = [&](AppState& s) {
        return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"Pulse startup fixture", WS_OVERLAPPEDWINDOW,
            -20000, -20000, 960, 720, nullptr, nullptr, wc.hInstance, &s);
    };

    auto owner = std::make_unique<AppState>();
    auto& first = *owner;
    Configure(first, root / L"first");
    const HWND primary = create(first);
    Check(primary != nullptr, "real WM_CREATE initializes the production window");
    if (!primary) { OleUninitialize(); return 2; }
    Check(!first.quickPreview.hwnd() && !first.operationWindow && !first.operationCenter &&
        !first.first_frame_drawn && first.deferred_startup_step == 0,
        "WM_CREATE leaves auxiliary windows behind the first frame");
    ShowWindow(primary, SW_SHOWNOACTIVATE);
    UpdateWindow(primary);
    Check(first.first_frame_drawn && first.deferred_startup_step == 0 && !first.operationWindow,
        "production WM_PAINT draws before any deferred startup step");

    auto child = std::make_unique<AppState>(first.core);
    auto& second = *child;
    Configure(second, root / L"second");
    const HWND secondary = create(second);
    Check(secondary != nullptr, "second window shares the process while owning deferred services");
    if (!secondary) { DestroyWindow(primary); child.reset(); owner.reset(); OleUninitialize(); return 2; }
    DestroyWindow(primary);
    Check(first.core.global_ui_owner == secondary && !first.hwnd && !first.operationWindow &&
        second.deferred_startup_step == 0, "closing the first window before deferred work transfers global ownership");

    Check(Wait([&] {
        ProcessPendingResults(second);
        return ActiveTab(second) && !ActiveTab(second)->loading;
    }, false), "early listing completes without running deferred startup messages");
    auto* tab = ActiveTab(second);
    if (tab && tab->CountBound() > 0) tab->SelectOnly(0);
    ToggleQuickPreview(second);
    Check(second.quickPreview.hwnd() && second.quickPreview.visible() && second.deferred_startup_step == 0,
        "Space action initializes and opens Quick Look before its deferred step");
    const HWND quick = second.quickPreview.hwnd();
    second.quickPreview.Close();
    PinAndShowOperationWindow(second);
    auto* operation = second.operationWindow.get();
    ToggleOperationCenter(second);
    auto* center = second.operationCenter.get();
    Check(operation && center, "early operation controls initialize their own windows");
    if (center) center->Hide(false);
    const bool hidden_finished = Wait([&] { return second.deferred_startup_step == 6; });
    Check(hidden_finished && second.quickPreview.hwnd() == quick &&
        second.operationWindow.get() == operation && second.operationCenter.get() == center,
        "real hidden startup timers finish all steps without replacing windows opened early");

    ShowWindow(secondary, SW_SHOWNOACTIVATE);
    UpdateWindow(secondary);
    second.ui_schedule.last_activity = GetTickCount64() - app::UiScheduleState::kQuietAfterMs;
    SyncUiScheduling(second);
    Check(second.ui_schedule.maintenance_ms == 500, "quiet visible window uses the 500 ms maintenance timer");
    second.addressLiveDue = GetTickCount64() + 40;
    SyncUiScheduling(second);
    Check(second.ui_schedule.deadline == second.addressLiveDue,
        "quiet maintenance preserves the independent search deadline");
    second.addressLiveDue = 0;
    SyncVisibleWatches(second);
    std::ofstream(root / L"second" / L"watch-wakeup.txt") << "wake immediately";
    Check(Wait([&] {
        std::lock_guard lock(second.notify_mu);
        return !second.notify_queue.empty();
    }, false), "real directory watcher queues a result without waiting for maintenance");
    MSG kick{};
    const bool watcher_kick = PeekMessageW(&kick, secondary, WM_UI_KICK, WM_UI_KICK, PM_REMOVE) != FALSE;
    Check(watcher_kick, "real directory watcher posts an immediate UI wakeup");
    if (watcher_kick) DispatchMessageW(&kick);
    Check(second.ui_schedule.maintenance_ms == 100,
        "background wake restores active maintenance immediately");
    Check(Wait([&] {
        const auto* current = ActiveTab(second);
        if (!current) return false;
        for (int i = 0; i < current->CountBound(); ++i)
            if (current->EntryAt(static_cast<size_t>(i)).name == L"watch-wakeup.txt") return true;
        return false;
    }), "real watcher wake applies the new file to the visible listing");
    ShowWindow(secondary, SW_HIDE);
    SyncUiScheduling(second);
    Check(second.ui_schedule.maintenance_ms == 1000, "hidden window uses the 1000 ms maintenance timer");
    second.folderSizes.TakeChanged();
    while (PeekMessageW(&kick, secondary, WM_UI_KICK, WM_UI_KICK, PM_REMOVE)) {}
    second.folderSizes.Invalidate((root / L"second").wstring());
    Check(PeekMessageW(&kick, secondary, WM_UI_KICK, WM_UI_KICK, PM_REMOVE) != FALSE,
        "folder-size invalidation wakes a hidden window immediately");
    second.exit_requested = true;
    SyncUiScheduling(second);
    Check(second.ui_schedule.maintenance_ms == 200, "shutdown retains its bounded 200 ms fallback");
    second.exit_requested = false;

    auto closing = std::make_unique<AppState>(first.core);
    Configure(*closing, root / L"first");
    const HWND closing_hwnd = create(*closing);
    Check(closing_hwnd != nullptr, "create early-shutdown fixture");
    if (closing_hwnd) {
        closing->exit_requested = true;
        SendMessageW(closing_hwnd, WM_TIMER, kTimerDeferredStartup, 0);
        EnsureQuickPreviewWindow(*closing);
        EnsureOperationWindow(*closing);
        InitializeOperationCenter(*closing);
        Check(closing->deferred_startup_step == 0 && !closing->quickPreview.hwnd() &&
            !closing->operationWindow && !closing->operationCenter,
            "early shutdown blocks delayed and on-demand auxiliary creation");
        DestroyWindow(closing_hwnd);
    }
    closing.reset();
    DestroyWindow(secondary);
    child.reset();
    owner.reset();
    OleUninitialize();
    std::printf("STARTUP IDLE failures=%d\n", failures);
    return failures ? 1 : 0;
}
