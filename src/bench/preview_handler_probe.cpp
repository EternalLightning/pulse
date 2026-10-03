#include "../ui/preview_handler_host.h"
#include <ole2.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <atomic>
#include <vector>

namespace pulse::ui {
void ResetPreviewHandlerOpenAttemptsForTest();
uint32_t PreviewHandlerOpenAttemptsForTest();
bool PreviewHandlerCanActivateIsolatedForTest(const std::wstring& path);
void ResetSlowPreviewProvidersForTest();
bool PreviewProviderCoolingDownForTest(const std::wstring& path);
void ResetOverlayPlaceCountsForTest();
uint32_t OverlayPlaceCallsForTest();
uint32_t OverlayPlaceDoneForTest();
}

namespace {

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct OpenBlock {
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<uint32_t> entered{0};
    ~OpenBlock() { if (release) CloseHandle(release); }
};

bool Lifecycle(HWND owner, bool permanent) {
    using Host = pulse::ui::PreviewHandlerHost;
    auto block = std::make_shared<OpenBlock>();
    Host::SetOpenHookForTest([block] {
        block->entered.fetch_add(1);
        WaitForSingleObject(block->release, INFINITE);
    });
    SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", L"20");
    SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", L"20");
    pulse::ui::ResetSlowPreviewProvidersForTest();
    const auto limit = Host::Resources().limit;
    const auto bounds = D2D1::RectF(16.0f, 16.0f, 380.0f, 640.0f);
    const auto bg = D2D1::ColorF(0.1f, 0.1f, 0.1f);
    const auto fg = D2D1::ColorF(0.9f, 0.9f, 0.9f);
    bool ok = true;
    double max_destroy = 0.0;
    std::vector<std::unique_ptr<Host>> active;
    for (uint32_t i = 0; i < limit; ++i) {
        auto host = std::make_unique<Host>();
        host->Sync(owner, bounds, L"active.pulse-cap-" + std::to_wstring(i), FILE_ATTRIBUTE_NORMAL,
                   1, 0, 0, true, bg, fg, true, true);
        active.push_back(std::move(host));
    }
    const auto active_deadline = GetTickCount64() + 3000;
    while (block->entered.load() < limit && GetTickCount64() < active_deadline) Sleep(1);
    const auto active_usage = Host::Resources();
    const bool active_cap = active_usage.live == limit && active_usage.retired == 0 &&
        !Host::CanHost(L"new.unrelated-extension") && !Host{}.CanHostPath(L"new.unrelated-extension") &&
        active.front()->CanHostPath(L"new.unrelated-extension");
    wprintf(L"[%s] full active pool sends new hosts to native fallback\n", active_cap ? L"PASS" : L"FAIL");
    ok &= active_cap;
    active.clear();
    SetEvent(block->release);
    const auto active_cleanup = GetTickCount64() + 5000;
    while (Host::Resources().live && GetTickCount64() < active_cleanup) Sleep(1);
    const bool active_reaped = Host::Resources().live == 0;
    ok &= active_reaped;
    if (!active_reaped) {
        Host::SetOpenHookForTest({});
        return false;
    }
    ResetEvent(block->release);
    block->entered.store(0);
    for (uint32_t i = 0; i < limit; ++i) {
        auto host = std::make_unique<Host>();
        const std::wstring path = L"blocked.pulse-life-" + std::to_wstring(i);
        host->Sync(owner, bounds, path, FILE_ATTRIBUTE_NORMAL, 1, 0, 0, true, bg, fg, true, true);
        const auto deadline = GetTickCount64() + 3000;
        while (block->entered.load() < i + 1 && GetTickCount64() < deadline) Sleep(1);
        ok &= block->entered.load() == i + 1;
        Sleep(25);
        host->Sync(owner, bounds, path, FILE_ATTRIBUTE_NORMAL, 1, 0, 0, true, bg, fg, true, true);
        ok &= Host::Resources().retired == i + 1;
        // Even erasing/expiring cooldown cannot retry an unresolved apartment.
        pulse::ui::ResetSlowPreviewProvidersForTest();
        ok &= pulse::ui::PreviewProviderCoolingDownForTest(path);
        host->Sync(owner, bounds, path, FILE_ATTRIBUTE_NORMAL, 2, 0, 0, true, bg, fg, true, true);
        ok &= block->entered.load() == i + 1;
        const auto before = std::chrono::steady_clock::now();
        host.reset();
        max_destroy = (std::max)(max_destroy, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - before).count());
    }
    for (int i = 0; i < 24; ++i) {
        Host denied;
        denied.Sync(owner, bounds, L"denied.pulse-new", FILE_ATTRIBUTE_NORMAL,
                    3, 0, 0, true, bg, fg, true, true);
        ok &= denied.state() == Host::State::Failed;
    }
    const auto full = Host::Resources();
    const bool capped = full.live == limit && full.retired == limit && block->entered.load() == limit;
    wprintf(L"[%s] process-wide apartment cap survives host destruction/cooldown reset "
            L"(live=%u retired=%u cap=%u)\n", capped ? L"PASS" : L"FAIL", full.live, full.retired, full.limit);
    wprintf(L"[%s] destructor never waits for provider (max %.2f ms)\n",
            max_destroy < 100.0 ? L"PASS" : L"FAIL", max_destroy);
    ok &= capped && max_destroy < 100.0;
    if (permanent) {
        wprintf(L"[INFO] %u permanently blocked apartments remain until process exit; not zero leakage\n", full.live);
    } else {
        SetEvent(block->release);
        const auto deadline = GetTickCount64() + 5000;
        while (Host::Resources().live && GetTickCount64() < deadline) Sleep(1);
        const bool reaped = Host::Resources().live == 0;
        wprintf(L"[%s] late-returning apartments are reclaimed after actual thread exit\n", reaped ? L"PASS" : L"FAIL");
        ok &= reaped;
        Host::SetOpenHookForTest([] {});
        Host fresh;
        fresh.Sync(owner, bounds, L"fresh.pulse-ready", FILE_ATTRIBUTE_NORMAL,
                   4, 0, 0, true, bg, fg, true, true);
        const auto ready_deadline = GetTickCount64() + 3000;
        while (fresh.state() == Host::State::Loading && GetTickCount64() < ready_deadline) Sleep(1);
        const bool answered = fresh.state() == Host::State::Failed;
        wprintf(L"[%s] a new apartment answers after recovered capacity\n", answered ? L"PASS" : L"FAIL");
        ok &= answered;
    }
    Host::SetOpenHookForTest({});
    SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", nullptr);
    SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", nullptr);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || ((wcscmp(argv[1], L"--selftest") == 0 ||
                      wcscmp(argv[1], L"--isolated-test") == 0) && argc < 3)) {
        wprintf(L"usage: pulse_preview_handler_probe.exe "
                L"[--selftest|--isolated-test] <file> or "
                L"--cooldown-test / --lifecycle-test / --permanent-test\n");
        return 1;
    }
    const bool lifecycle_test = wcscmp(argv[1], L"--lifecycle-test") == 0;
    const bool permanent_test = wcscmp(argv[1], L"--permanent-test") == 0;
    const bool cooldown_test = wcscmp(argv[1], L"--cooldown-test") == 0;
    const bool self_test = argc >= 3 && wcscmp(argv[1], L"--selftest") == 0;
    const bool isolated_test = argc >= 3 && wcscmp(argv[1], L"--isolated-test") == 0;
    const std::wstring path = argv[(self_test || isolated_test) ? 2 : 1];
    wprintf(L"probe file=%s\n", path.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx hr=0x%08X\n", static_cast<unsigned>(hr));
        return 2;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PulsePreviewProbeOwner";
    RegisterClassExW(&wc);

    HWND owner = CreateWindowExW(0, wc.lpszClassName, L"Pulse preview probe",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 80, 80, 420, 860, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    if (!owner) {
        wprintf(L"CreateWindow owner failed error=%lu\n", GetLastError());
        CoUninitialize();
        return 3;
    }
    UpdateWindow(owner);
    if (lifecycle_test || permanent_test) {
        const bool ok = Lifecycle(owner, permanent_test);
        DestroyWindow(owner);
        CoUninitialize();
        return ok ? 0 : 7;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const BOOL have_fad = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);
    const DWORD attrs = have_fad ? fad.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
    const uint64_t modified = have_fad
        ? (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32)
          | fad.ftLastWriteTime.dwLowDateTime
        : 0;
    const uint64_t size = have_fad
        ? (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow
        : 0;
    wprintf(L"attrs=0x%08X exist=%d size=%llu canhost=%d\n",
            attrs, have_fad ? 1 : 0, static_cast<unsigned long long>(size),
            pulse::ui::PreviewHandlerHost::CanHost(path) ? 1 : 0);
    if (isolated_test) {
        const bool isolated = pulse::ui::PreviewHandlerCanActivateIsolatedForTest(path);
        wprintf(L"[%s] preview handler supports isolated local-server activation\n",
                isolated ? L"PASS" : L"FAIL");
        DestroyWindow(owner);
        CoUninitialize();
        return isolated ? 0 : 5;
    }

    const D2D1_RECT_F bounds = D2D1::RectF(16.0f, 16.0f, 380.0f, 640.0f);
    const D2D1_COLOR_F bg = D2D1::ColorF(0.12f, 0.12f, 0.12f);
    const D2D1_COLOR_F fg = D2D1::ColorF(0.92f, 0.92f, 0.92f);
    if (cooldown_test) {
        // Delay before provider activation, so this regression needs no installed
        // Office/PDF handler or real files. B is queued while A is still opening.
        const std::wstring first = L"pulse-cooldown-a.pulse-test-a";
        const std::wstring second = L"pulse-cooldown-b.pulse-test-b";
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"1200");
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", L"200");
        pulse::ui::ResetSlowPreviewProvidersForTest();
        pulse::ui::ResetPreviewHandlerOpenAttemptsForTest();
        bool started = false;
        bool queued_before_timeout = false;
        bool first_cooled = false;
        bool second_cooled = false;
        {
            pulse::ui::PreviewHandlerHost host;
            host.Sync(owner, bounds, first, FILE_ATTRIBUTE_NORMAL, 1, 0, 0,
                      true, bg, fg, true, true);
            const ULONGLONG start_deadline = GetTickCount64() + 1000;
            while (GetTickCount64() < start_deadline) {
                if (pulse::ui::PreviewHandlerOpenAttemptsForTest() > 0) {
                    started = true;
                    break;
                }
                Sleep(1);
            }
            if (started) {
                host.Sync(owner, bounds, second, FILE_ATTRIBUTE_NORMAL, 2, 0, 0,
                          true, bg, fg, true, true);
                queued_before_timeout =
                    !pulse::ui::PreviewProviderCoolingDownForTest(first) &&
                    !pulse::ui::PreviewProviderCoolingDownForTest(second);
                const ULONGLONG deadline = GetTickCount64() + 700;
                while (GetTickCount64() < deadline) {
                    host.Sync(owner, bounds, second, FILE_ATTRIBUTE_NORMAL, 2, 0, 0,
                              true, bg, fg, true, true);
                    first_cooled = pulse::ui::PreviewProviderCoolingDownForTest(first);
                    second_cooled = pulse::ui::PreviewProviderCoolingDownForTest(second);
                    if (first_cooled || second_cooled) break;
                    Sleep(10);
                }
            }
        }
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", nullptr);
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", nullptr);
        const bool ok = started && queued_before_timeout && first_cooled && !second_cooled;
        wprintf(L"[%s] switching A to B cools only the blocked A "
                L"(started=%d, queued=%d, A=%d, B=%d)\n",
                ok ? L"PASS" : L"FAIL", started, queued_before_timeout, first_cooled, second_cooled);
        // Pump the owner while the retired worker finishes window cleanup.
        const ULONGLONG cleanup_deadline = GetTickCount64() + 1400;
        while (GetTickCount64() < cleanup_deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        pulse::ui::ResetSlowPreviewProvidersForTest();
        DestroyWindow(owner);
        CoUninitialize();
        return ok ? 0 : 6;
    }
    if (self_test) {
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"1500");
        // This block only measures that the owner thread never waits on the
        // apartment, so the stall watchdog is kept out of its way.
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", L"60000");
        pulse::ui::ResetPreviewHandlerOpenAttemptsForTest();
        double max_sync_ms = 0.0;
        double max_reposition_ms = 0.0;
        double reset_ms = 0.0;
        double destroy_ms = 0.0;
        {
            auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
            host->SetNotifyWindow(owner);
            for (uint64_t generation = 1; generation <= 64; ++generation) {
                const auto start = std::chrono::steady_clock::now();
                host->Sync(owner, bounds, path, attrs, generation, modified, size,
                           true, bg, fg, true, generation == 64);
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                max_sync_ms = (std::max)(max_sync_ms, elapsed);
            }
            // Let the worker enter the injected slow COM section.
            Sleep(180);
            for (int i = 0; i < 64; ++i) {
                const auto start = std::chrono::steady_clock::now();
                host->Reposition();
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                max_reposition_ms = (std::max)(max_reposition_ms, elapsed);
            }
            const auto reset_start = std::chrono::steady_clock::now();
            host->Reset();
            reset_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - reset_start).count();
            const auto destroy_start = std::chrono::steady_clock::now();
            host.reset();
            destroy_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - destroy_start).count();
        }
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", nullptr);
        SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", nullptr);
        const bool sync_ok = max_sync_ms < 50.0;
        const bool reposition_ok = max_reposition_ms < 50.0;
        const bool reset_ok = reset_ms < 50.0;
        const bool destroy_ok = destroy_ms < 350.0;
        const uint32_t open_attempts = pulse::ui::PreviewHandlerOpenAttemptsForTest();
        const bool coalesced = open_attempts == 1;
        wprintf(L"[%s] COM preview Sync remains non-blocking (max %.2f ms)\n",
                sync_ok ? L"PASS" : L"FAIL", max_sync_ms);
        wprintf(L"[%s] preview reposition remains non-blocking (max %.2f ms)\n",
                reposition_ok ? L"PASS" : L"FAIL", max_reposition_ms);
        wprintf(L"[%s] COM preview Reset remains non-blocking (%.2f ms)\n",
                reset_ok ? L"PASS" : L"FAIL", reset_ms);
        wprintf(L"[%s] slow handler cannot hang destruction (%.2f ms)\n",
                destroy_ok ? L"PASS" : L"FAIL", destroy_ms);
        wprintf(L"[%s] rapid switches coalesce to one open attempt (%u)\n",
                coalesced ? L"PASS" : L"FAIL", open_attempts);

        // Moving the window moves the pane, and the preview that lives in the
        // overlay window has to land on the new screen position right away: a
        // preview that follows late visibly trails the window while the user
        // drags it. The Office and PDF providers own a window in another process,
        // which is exactly the case that used to lag.
        bool follow_ok = true;
        if (!pulse::ui::PreviewHandlerHost::CanHost(path)) {
            wprintf(L"[SKIP] overlay follow needs a file with a system preview handler\n");
        } else {
            double worst_follow_ms = 0.0;
            double total_follow_ms = 0.0;
            int steps = 0;
            int missed = 0;
            bool shown = false;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                host->Sync(owner, bounds, path, attrs, 31, modified, size, true, bg, fg, true, true);
                const ULONGLONG shown_deadline = GetTickCount64() + 20000;
                while (GetTickCount64() < shown_deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (host->state() == pulse::ui::PreviewHandlerHost::State::Shown) {
                        shown = true;
                        break;
                    }
                    if (host->state() == pulse::ui::PreviewHandlerHost::State::Failed) break;
                    Sleep(10);
                }
                const HWND overlay = host->overlay_window_for_test();
                RECT rest_owner{};
                RECT rest_overlay{};
                if (shown && overlay && GetWindowRect(owner, &rest_owner) &&
                    GetWindowRect(overlay, &rest_overlay)) {
                    // The overlay keeps the same client-relative bounds, so it has
                    // to move by exactly as much as the owner moved.
                    const LONG offset = rest_overlay.left - rest_owner.left;
                    // One step per frame of a drag, with no settling in between:
                    // this is the rate the pane sees while the window is moving.
                    pulse::ui::ResetOverlayPlaceCountsForTest();
                    for (int step = 1; step <= 16; ++step) {
                        const LONG want = rest_owner.left + 12 * step;
                        SetWindowPos(owner, nullptr, want, rest_owner.top, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        host->Reposition(); // what the app does from WM_MOVE
                        const auto start = std::chrono::steady_clock::now();
                        double elapsed = -1.0;
                        RECT now{};
                        for (;;) {
                            const double waited = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - start).count();
                            if (GetWindowRect(overlay, &now) && now.left == want + offset) {
                                elapsed = waited;
                                break;
                            }
                            if (waited > 500.0) break;
                        }
                        if (step <= 3 && elapsed < 0.0) {
                            wprintf(L"    step %d want_overlay_left=%ld actual=%ld visible=%d\n",
                                    step, want + offset, now.left,
                                    IsWindowVisible(overlay) ? 1 : 0);
                        }
                        ++steps;
                        if (elapsed < 0.0) {
                            ++missed;
                        } else {
                            worst_follow_ms = (std::max)(worst_follow_ms, elapsed);
                            total_follow_ms += elapsed;
                        }
                    }
                }
                host->Reset();
                host.reset();
                Sleep(200);
            }
            const double average = steps > missed ? total_follow_ms / (steps - missed) : 0.0;
            follow_ok = shown && steps == 16 && missed == 0 && worst_follow_ms < 40.0;
            wprintf(L"[%s] the overlay follows a moving owner (%d steps, shown=%d, "
                    L"worst %.1f ms, average %.1f ms, missed %d, placements %u/%u)\n",
                    follow_ok ? L"PASS" : L"FAIL", steps, shown ? 1 : 0,
                    worst_follow_ms, average, missed,
                    pulse::ui::OverlayPlaceDoneForTest(), pulse::ui::OverlayPlaceCallsForTest());
        }

        // A provider that never comes back from its open used to stall this and
        // every later preview, because the apartment that opens it is also the
        // one that owns the overlay. The stalled apartment must be retired so the
        // pane falls back instead of waiting, and a fresh apartment must still
        // serve the next selection.
        bool stall_ok = true;
        if (!pulse::ui::PreviewHandlerHost::CanHost(path)) {
            wprintf(L"[SKIP] stalled provider recovery needs a file with a system preview handler\n");
        } else {
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"1200");
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", L"200");
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", L"200");
            double max_stall_sync_ms = 0.0;
            bool fell_back = false;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                const ULONGLONG deadline = GetTickCount64() + 4000;
                // The pane evaluates CanHost once per selection and hands the
                // answer straight to Sync, so the identity stays put.
                for (int attempt = 0; attempt < 400 && GetTickCount64() < deadline; ++attempt) {
                    const bool can_host = pulse::ui::PreviewHandlerHost::CanHost(path);
                    const auto start = std::chrono::steady_clock::now();
                    host->Sync(owner, bounds, path, attrs, 7, modified, size, true, bg, fg, can_host);
                    max_stall_sync_ms = (std::max)(max_stall_sync_ms,
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count());
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (!can_host) {
                        fell_back = true;
                        break;
                    }
                    Sleep(10);
                }
                host->Reset();
                host.reset();
                // The retired apartment returns from its provider call and
                // unloads on its own; give it that chance before the owner dies.
                Sleep(1400);
            }
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", nullptr);
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", nullptr);
            pulse::ui::ResetSlowPreviewProvidersForTest();
            stall_ok = fell_back && max_stall_sync_ms < 50.0;
            wprintf(L"[%s] stalled handler open is retired instead of blocking the pane "
                    L"(fell back=%d, max sync %.2f ms)\n",
                    stall_ok ? L"PASS" : L"FAIL", fell_back ? 1 : 0, max_stall_sync_ms);

            // With the cooldown lifted and a budget that fits the injected delay
            // a fresh apartment completes the open again: later selections are
            // served instead of queueing behind the stalled one.
            bool served = false;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                const ULONGLONG deadline = GetTickCount64() + 10000;
                for (int attempt = 0; attempt < 1000 && GetTickCount64() < deadline; ++attempt) {
                    host->Sync(owner, bounds, path, attrs, 7, modified, size, true, bg, fg, true);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    const auto state = host->state();
                    if (state == pulse::ui::PreviewHandlerHost::State::Shown ||
                        state == pulse::ui::PreviewHandlerHost::State::Failed) {
                        served = true;
                        break;
                    }
                    Sleep(10);
                }
                host->Reset();
                host.reset();
                Sleep(300);
            }
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", nullptr);
            pulse::ui::ResetSlowPreviewProvidersForTest();
            stall_ok = stall_ok && served;
            wprintf(L"[%s] a later preview is served by a fresh apartment (finished=%d)\n",
                    served ? L"PASS" : L"FAIL", served ? 1 : 0);
        }

        // Slow is not the same as stuck. A provider that needs longer than the
        // retire budget to start - an Office preview starts its application the
        // first time - used to be given up on while the pane was still showing
        // that very file, which left the preview with nothing but the failure
        // placeholder. Only a request the pane has moved on from may cut it off.
        bool slow_ok = true;
        if (!pulse::ui::PreviewHandlerHost::CanHost(path)) {
            wprintf(L"[SKIP] the slow provider grace needs a file with a system preview handler\n");
        } else {
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"2500");
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", L"300");
            double shown_ms = 0.0;
            bool shown = false;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                const auto start = std::chrono::steady_clock::now();
                const ULONGLONG deadline = GetTickCount64() + 12000;
                // The same request over and over: this is a pane repainting while
                // the user keeps looking at the file.
                for (int attempt = 0; attempt < 1200 && GetTickCount64() < deadline; ++attempt) {
                    host->Sync(owner, bounds, path, attrs, 9, modified, size, true, bg, fg, true);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (host->state() == pulse::ui::PreviewHandlerHost::State::Shown) {
                        shown = true;
                        shown_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();
                        break;
                    }
                    Sleep(10);
                }
                host->Reset();
                host.reset();
                Sleep(300);
            }
            // A provider that only needed more time must not have been cooled
            // down: the pane keeps handing it this file.
            const bool kept = pulse::ui::PreviewHandlerHost::CanHost(path);
            pulse::ui::ResetSlowPreviewProvidersForTest();
            slow_ok = shown && kept;
            wprintf(L"[%s] a slow provider still serves the selection on screen "
                    L"(shown=%d at %.0f ms, still used=%d)\n",
                    slow_ok ? L"PASS" : L"FAIL", shown ? 1 : 0, shown_ms, kept ? 1 : 0);

            // The file the user moved on to does not wait for it: the apartment is
            // retired on the short budget and the cooldown sends the pane to its
            // fallback instead of queueing behind the provider.
            bool left_behind = false;
            double fallback_ms = 0.0;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                // Open one file and let the provider get stuck in it.
                host->Sync(owner, bounds, path, attrs, 12, modified, size, true, bg, fg, true);
                const ULONGLONG opened = GetTickCount64() + 1000;
                while (GetTickCount64() < opened) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    Sleep(10);
                }
                const auto start = std::chrono::steady_clock::now();
                const ULONGLONG deadline = GetTickCount64() + 4000;
                for (int attempt = 0; attempt < 400 && GetTickCount64() < deadline; ++attempt) {
                    // Same file, new generation: the paint that follows a
                    // selection replacing the one being opened.
                    host->Sync(owner, bounds, path, attrs, 13, modified, size, true, bg, fg, true);
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (!pulse::ui::PreviewHandlerHost::CanHost(path)) {
                        left_behind = true;
                        fallback_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start).count();
                        break;
                    }
                    Sleep(10);
                }
                host->Reset();
                host.reset();
                Sleep(300);
            }
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_OPEN_BUDGET_MS", nullptr);
            pulse::ui::ResetSlowPreviewProvidersForTest();
            slow_ok = slow_ok && left_behind;
            wprintf(L"[%s] a superseded selection retires the stuck provider "
                    L"(fell back=%d at %.0f ms)\n",
                    left_behind ? L"PASS" : L"FAIL", left_behind ? 1 : 0, fallback_ms);

            // A provider that was given up on for being slow must not stay out of
            // reach once it answers: the open that was retired can still come
            // back, and the pane should get the handler again instead of the "no
            // provider" placeholder for the rest of the cooldown.
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", L"1200");
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", L"200");
            bool cooled_down = false;
            bool usable_again = false;
            {
                auto host = std::make_unique<pulse::ui::PreviewHandlerHost>();
                host->SetNotifyWindow(owner);
                host->Sync(owner, bounds, path, attrs, 15, modified, size, true, bg, fg, true);
                const ULONGLONG cooling = GetTickCount64() + 900;
                while (GetTickCount64() < cooling) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    Sleep(10);
                }
                // One repaint of the same request reaches the watchdog without
                // publishing anything new, so the retired open is left to finish.
                host->Sync(owner, bounds, path, attrs, 15, modified, size, true, bg, fg, true);
                cooled_down = !pulse::ui::PreviewHandlerHost::CanHost(path);
                const ULONGLONG deadline = GetTickCount64() + 8000;
                while (GetTickCount64() < deadline) {
                    MSG message{};
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if (pulse::ui::PreviewHandlerHost::CanHost(path)) {
                        usable_again = true;
                        break;
                    }
                    Sleep(10);
                }
                host->Reset();
                host.reset();
                Sleep(200);
            }
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_TEST_DELAY_MS", nullptr);
            SetEnvironmentVariableW(L"PULSE_PREVIEW_HANDLER_SLOW_OPEN_BUDGET_MS", nullptr);
            pulse::ui::ResetSlowPreviewProvidersForTest();
            slow_ok = slow_ok && cooled_down && usable_again;
            wprintf(L"[%s] a provider that answered clears its cooldown "
                    L"(cooled down=%d, usable again=%d)\n",
                    (cooled_down && usable_again) ? L"PASS" : L"FAIL",
                    cooled_down ? 1 : 0, usable_again ? 1 : 0);
        }

        DestroyWindow(owner);
        CoUninitialize();
        return sync_ok && reposition_ok && reset_ok && destroy_ok && coalesced && stall_ok &&
                slow_ok && follow_ok ? 0 : 4;
    }

    pulse::ui::PreviewHandlerHost host;
    host.SetNotifyWindow(owner);
    host.Sync(owner, bounds, path, attrs, 1, modified, size, true, bg, fg, true);

    const ULONGLONG deadline = GetTickCount64() + 6000;
    MSG msg{};
    bool running = true;
    while (running && GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (running) Sleep(16);
    }
    wprintf(L"state=%d (0 idle, 1 loading, 2 shown, 3 failed)\n",
            static_cast<int>(host.state()));
    host.Reset();
    DestroyWindow(owner);
    CoUninitialize();
    return 0;
}
