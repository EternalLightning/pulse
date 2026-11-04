// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/app_ui_schedule.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <filesystem>
#include <string_view>

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    const bool latency_two_only = argc == 2 && std::string_view(argv[1]) == "--latency-two-only";
    SetEnvironmentVariableW(L"PULSE_FRAME_PACING", L"waitable");
    SetEnvironmentVariableW(L"PULSE_FRAME_LATENCY", latency_two_only ? L"2" : L"1");
    wchar_t fixture_root[32768]{};
    const DWORD fixture_length = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT",
        fixture_root, ARRAYSIZE(fixture_root));
    const bool specified_root = fixture_length && fixture_length < ARRAYSIZE(fixture_root);
    const auto root = specified_root ? std::filesystem::path(fixture_root) :
        std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"frame-pacing-" + std::to_wstring(GetCurrentProcessId())));
    if (!specified_root) std::filesystem::create_directories(root);
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"PulseFramePacingFixture";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    auto first = std::make_unique<AppState>();
    auto second = std::make_unique<AppState>(first->core);
    const auto create = [&](AppState& s, int x) {
        s.isolatedTest = s.safeMode = true;
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.core.shared_preferences_loaded = true;
        s.open_path = root.wstring();
        const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"Pulse frame pacing fixture", WS_OVERLAPPEDWINDOW,
            x, 10, 640, 480, nullptr, nullptr, wc.hInstance, &s);
        if (hwnd) {
            KillTimer(hwnd, kTimerUi);
            KillTimer(hwnd, kTimerUiDeadline);
            KillTimer(hwnd, kTimerDeferredStartup);
        }
        return hwnd;
    };
    const HWND a = create(*first, 10), b = create(*second, 680);
    check(a && b, "initialize two production windows on the shared device");
    if (!a || !b) {
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
        second.reset(); first.reset(); OleUninitialize(); return 2;
    }
    const HANDLE first_handle = first->compositor.FrameLatencyWaitableObject();
    const HANDLE second_handle = second->compositor.FrameLatencyWaitableObject();
    check(first_handle && second_handle, "experimental swap chains expose independent wait handles");
    if (latency_two_only && first_handle && second_handle) {
        check(WaitForSingleObject(first_handle, 1000) == WAIT_OBJECT_0 &&
            WaitForSingleObject(second_handle, 1000) == WAIT_OBJECT_0,
            "depth-two swap chains verify their queue depth and provide initial permits");
        // Depth two can provide one additional initial permit. Exhaust it
        // without Present so both production paints must defer to the pump.
        WaitForSingleObject(first_handle, 0);
        WaitForSingleObject(second_handle, 0);
        ShowWindow(a, SW_SHOWNOACTIVATE);
        ShowWindow(b, SW_SHOWNOACTIVATE);
        InvalidateRect(a, nullptr, FALSE);
        InvalidateRect(b, nullptr, FALSE);
        UpdateWindow(a);
        UpdateWindow(b);
        check(first->ui_schedule.frame_pending && second->ui_schedule.frame_pending &&
            !GetUpdateRect(a, nullptr, FALSE) && !GetUpdateRect(b, nullptr, FALSE),
            "depth-two production paints defer without blocking or paint spinning");
        constexpr UINT input = WM_APP + 999;
        PostThreadMessageW(GetCurrentThreadId(), input, 0, 0);
        MSG message{};
        check(WaitForUiFrameOrMessage(first->core, 0) == WAIT_OBJECT_0 + 2 &&
            PeekMessageW(&message, nullptr, input, input, PM_REMOVE),
            "depth-two pending windows keep queued UI messages available");
    } else if (!latency_two_only && first_handle && second_handle) {
        // Consume each initial permit without presenting: neither window should
        // block message dispatch while waiting for its next permit.
        check(WaitForSingleObject(first_handle, 1000) == WAIT_OBJECT_0 &&
            WaitForSingleObject(second_handle, 1000) == WAIT_OBJECT_0,
            "both swap chains signal before their first render");
        ShowWindow(a, SW_SHOWNOACTIVATE);
        ShowWindow(b, SW_SHOWNOACTIVATE);
        BeginUiFrame(*first);
        BeginUiFrame(*second);
        InvalidateRect(a, nullptr, FALSE);
        InvalidateRect(b, nullptr, FALSE);
        first->scrollAnimating = true;
        UpdateWindow(a);
        UpdateWindow(b);
        check(first->ui_schedule.frame_pending && second->ui_schedule.frame_pending &&
            !GetUpdateRect(a, nullptr, FALSE) && !GetUpdateRect(b, nullptr, FALSE),
            "production WM_PAINT defers unsignaled scrolling frames without paint spinning");
        first->scrollAnimating = false;

        constexpr UINT input = WM_APP + 999;
        PostThreadMessageW(GetCurrentThreadId(), input, 0, 0);
        const DWORD wake = WaitForUiFrameOrMessage(first->core, 0);
        MSG message{};
        check(wake == WAIT_OBJECT_0 + 2 &&
            PeekMessageW(&message, nullptr, input, input, PM_REMOVE),
            "queued input wakes the production wait while both frames are pending");

        Render(*second);
        const auto until = GetTickCount64() + 2000;
        while (!second->ui_schedule.frame_ready && GetTickCount64() < until) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_PAINT && (message.hwnd == a || message.hwnd == b)) {
                    // Keep the intentionally pending main frames for the outer
                    // wait; child paints and other input still need dispatch.
                    ValidateRect(message.hwnd, nullptr);
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            WaitForUiFrameOrMessage(first->core, 20);
            Sleep(1);
        }
        check(second->ui_schedule.frame_ready && !first->ui_schedule.frame_ready &&
            GetUpdateRect(b, nullptr, FALSE),
            "a ready second window wakes independently and restores its invalid region");
        ShowWindow(a, SW_HIDE);
        WaitForUiFrameOrMessage(first->core, 0);
        check(!first->ui_schedule.frame_pending, "hidden pending window leaves the wait set");
        UpdateWindow(b);
        // Directory completions can start a real content animation during the
        // input pump above. Run production scheduling through its endpoint.
        const auto settle_until = GetTickCount64() + 2000;
        while ((second->ui_schedule.animation_active || second->ui_schedule.frame_pending ||
                GetUpdateRect(b, nullptr, FALSE)) && GetTickCount64() < settle_until) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (second->ui_schedule.animation_active || second->ui_schedule.frame_pending ||
                GetUpdateRect(b, nullptr, FALSE))
                WaitForUiFrameOrMessage(first->core, 20);
        }
        check(!second->ui_schedule.animation_active && !second->ui_schedule.frame_pending &&
            !GetUpdateRect(b, nullptr, FALSE),
            "settled frame consumes its permit and stops requesting paint");

        ShowWindow(a, SW_SHOWNOACTIVATE);
        SendMessageW(a, WM_ENTERSIZEMOVE, 0, 0);
        check(first->ui_schedule.frame_wait_suspended && BeginUiFrame(*first),
            "native sizing loop uses Present pacing without relying on the outer pump");
        SendMessageW(a, WM_EXITSIZEMOVE, 0, 0);
        check(!first->ui_schedule.frame_wait_suspended,
            "leaving the native sizing loop restores waitable pacing");

        ShowWindow(a, SW_SHOWMINNOACTIVE);
        first->ui_schedule.frame_pending = true;
        first->ui_schedule.frame_wait_started = GetTickCount64();
        WaitForUiFrameOrMessage(first->core, 0);
        check(!first->ui_schedule.frame_pending, "minimized pending window leaves the wait set");

        ShowWindow(a, SW_SHOWNOACTIVATE);
        const int width = first->compositor.Width(), height = first->compositor.Height();
        first->compositor.Resize(width + 1, height + 1);
        check(first->compositor.FrameLatencyWaitableObject() == first_handle &&
            first->compositor.CanScheduleAnimationFrame(),
            "resize preserves the waitable flag, handle and render target");
        first->compositor.NotifyDeviceLost(DXGI_ERROR_DEVICE_RESET);
        check(!first->compositor.FrameLatencyWaitableObject(), "device loss removes the old handle from scheduling");
        Render(*first, true);
        check(!first->compositor.NeedsRecovery() && first->compositor.FrameLatencyWaitableObject(),
            "production render recreates the waitable swap chain after device loss");

        first->ui_schedule.frame_pending = true;
        first->ui_schedule.frame_wait_started = GetTickCount64() - 1001;
        WaitForUiFrameOrMessage(first->core, 0);
        check(!first->compositor.WaitableFramePacingEnabled() && GetUpdateRect(a, nullptr, FALSE),
            "stalled handle falls back to Present pacing and restores pending paint");
        StopUiScheduling(*first);
        check(!first->ui_schedule.frame_pending && !first->ui_schedule.frame_ready,
            "shutdown clears pending waits and acquired permits");
    }
    DestroyWindow(b);
    DestroyWindow(a);
    second.reset(); first.reset();
    SetEnvironmentVariableW(L"PULSE_FRAME_PACING", nullptr);
    SetEnvironmentVariableW(L"PULSE_FRAME_LATENCY", nullptr);
    OleUninitialize();
    std::printf("FRAME PACING failures=%d\n", failures);
    return failures ? 1 : 0;
}
