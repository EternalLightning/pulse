// SPDX-License-Identifier: Apache-2.0
#include "../app/app_internal.h"
#include "../app/app_ui_schedule.h"
#include "../app/frame_profile.h"
#include "../app/cloud_root_catalog.h"
#include "../common/user_storage.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include "status_repaint_cases.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int main(int argc, char** argv) {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    compat::EnableDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"idle-render-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root / L"files");
    std::ofstream(root / L"files" / L"sample.txt") << "idle fixture";
    storage::OverrideDefaultRootForTesting((root / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"PulseIdleRenderFixture";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = s.safeMode = true;
    s.core.shared_preferences_loaded = true;
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.open_path = (root / L"files").wstring();
    const bool status_only = argc == 2 && std::string_view(argv[1]) == "--status-changes-only";
    const bool cloud_idle = argc == 2 && std::string_view(argv[1]) == "--cloud-idle-only";
    if (status_only || cloud_idle) {
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_CLOUD_SYNC", L"sample.txt=6;unseen.txt=7");
    }
    const HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse idle rendering fixture", WS_OVERLAPPEDWINDOW,
        40, 40, 960, 720, nullptr, nullptr, wc.hInstance, &s);
    if (!hwnd) { state.reset(); OleUninitialize(); return 2; }
    if (status_only || cloud_idle) {
        const auto until = GetTickCount64() + 5000;
        while (!app::CloudRootCatalog() && GetTickCount64() < until) Sleep(1);
        app::PublishCloudFilesRoots({ActiveTab(s)->current_path});
        ActiveTab(s)->view_mode = ui::ViewMode::Details;
        while (ActiveTab(s)->loading && GetTickCount64() < until) {
            ProcessPendingResults(s);
            Sleep(1);
        }
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    int failures = 0;
    if (status_only) {
        bench::StatusRepaintCases(s, [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
            if (!ok) ++failures;
        });
        DestroyWindow(hwnd);
        state.reset();
        OleUninitialize();
        return failures ? 1 : 0;
    }
    const auto sample = [&](const char* label, uint64_t duration, bool verify) {
        unsigned paints = 0, draws = 0, presents = 0, timers = 0;
        app::FrameProfileSample frame;
        app::ObserveFrameProfile observe(frame);
        app::ObserveFrameDrawProfile hooks({&draws,
            [](void* count) noexcept { ++*static_cast<unsigned*>(count); }, nullptr});
        const auto until = GetTickCount64() + duration;
        while (GetTickCount64() < until) {
            MSG msg{};
            while (GetTickCount64() < until && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.hwnd == hwnd && msg.message == WM_PAINT) ++paints;
                if (msg.hwnd == hwnd && msg.message == WM_TIMER && msg.wParam == kTimerUi) ++timers;
                frame = {};
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (frame.complete && frame.presented) ++presents;
            }
            WaitForUiFrameOrMessage(s.core, 10);
        }
        std::printf("%s: %llu ms, paints=%u draws=%u presents=%u maintenance=%u animation=%d interval=%u\n",
            label, static_cast<unsigned long long>(duration), paints, draws, presents, timers,
            s.ui_schedule.animation_active, s.ui_schedule.maintenance_ms);
        if (verify) {
            const bool ok = draws == 0 && !s.ui_schedule.animation_active && timers > 0;
            std::printf("[%s] %s settles with maintenance enabled\n", ok ? "PASS" : "FAIL", label);
            if (!ok) ++failures;
        }
    };
    sample(cloud_idle ? "cloud folder settle" : "folder settle", 6500, false);
    sample(cloud_idle ? "cloud folder idle" : "folder idle", 5000, true);
    if (!cloud_idle) {
        NavigateTo(s, app::MakeHomePath());
        sample("home settle", 6500, false);
        sample("home idle", 5000, true);
        OpenSettingsTab(s, 0);
        sample("settings settle", 6500, false);
        sample("settings idle", 5000, true);
    }
    ShowWindow(hwnd, SW_HIDE);
    sample("hidden settle", 500, false);
    sample("hidden idle", 2500, true);
    DestroyWindow(hwnd);
    state.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
