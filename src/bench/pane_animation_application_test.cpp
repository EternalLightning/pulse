#include "../app/app_internal.h"
#include "../app/pane_animations.h"
#include "../app/app_ui_schedule.h"
#include "../app/app_visual_animation.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    };
    const std::filesystem::path root = L"bench_data/pane_animation_ui";
    std::filesystem::create_directories(root);
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = false;
    // Hidden capture skips flip-chain presentation so snapshots read the drawn buffer.
    s.shot.active = true;
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Pane animation fixture",
        WS_POPUP, 0, 0, 760, 540, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize hidden animation compositor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        owned.reset();
        OleUninitialize();
        return 1;
    }
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = std::filesystem::absolute(root).wstring();
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(1);
    entries->front().name = L"Animation sample.txt";
    entries->front().attrs = FILE_ATTRIBUTE_NORMAL;
    tab.snapshot = entries;
    tab.loading = false;

    check(!Render(s), "initial production render settles without a frame chain");
    ShowFilterEditor(s);
    check(s.filterEditing && s.filterFocusPending && s.hwndFilterEdit,
          "opening creates the edit and defers focus until expansion completes");
    check(Render(s), "production render starts the filter and requests another frame");
    s.pane->filter_animation_start = app::PaneAnimationTimeMs() - 70;
    check(Render(s) && s.pane->filter_expand > 0 && s.pane->filter_expand < 1,
          "production render advances the filter without a timer message");
    WndProcImpl(s.hwnd, WM_PAINT, 0, 0);
    check(!GetUpdateRect(s.hwnd, nullptr, FALSE), "hidden window never self-invalidates for animation");
    s.pane->filter_animation_start = app::PaneAnimationTimeMs() - 140;
    check(!Render(s) && s.pane->filter_expand == 1, "production render stops requesting frames at the endpoint");
    check(!s.filterFocusPending, "last animation frame completes edit handoff without a maintenance tick");
    HideFilterEditor(s, true);
    check(Render(s), "closing an empty filter schedules collapse");
    s.pane->filter_animation_start = app::PaneAnimationTimeMs() - 140;
    check(!Render(s) && s.pane->filter_expand == 0, "collapse terminates in the production render path");

    auto root_entries = std::make_shared<std::vector<fs::DirEntry>>(1);
    root_entries->front().name = L"Drive";
    root_entries->front().full_path = LR"(C:\)";
    root_entries->front().is_dir = true;
    tab.SetSnapshot(root_entries);
    tab.SelectOnly(0);
    check(BuildVm(s, false).selection_has_drive_root, "view model preserves drive-root toolbar protection");
    tab.ClearSelection();
    check(!BuildVm(s, false).selection_has_drive_root, "view model clears protection when selection changes");
    tab.SetSnapshot(entries);
    const auto sample_tick = s.processSampleTick;
    Render(s);
    check(s.processSampleTick == sample_tick && s.fpsWindow.empty(),
          "hidden performance display skips process sampling and FPS history");
    s.showFps = true;
    Render(s);
    check(s.processSampleTick != 0 && !s.fpsWindow.empty(),
          "enabling performance display resumes process sampling and FPS history");
    s.showFps = false;

    const uint64_t wall_now = GetTickCount64();
    s.addressLiveDue = wall_now + 80;
    s.addressHistoryDue = wall_now + 400;
    s.addressLiveContext = L"pulse:stale-search";
    s.addressHistoryPath = L"pulse:stale-history";
    WndProc(s.hwnd, WM_NULL, 0, 0);
    check(s.ui_schedule.deadline == s.addressLiveDue && s.ui_schedule.maintenance_ms == 200,
          "message completion arms the earliest deadline separately from hidden maintenance");
    s.addressLiveDue = wall_now + 160;
    WndProc(s.hwnd, WM_NULL, 0, 0);
    check(s.ui_schedule.deadline == wall_now + 160, "typing replaces the debounce deadline");
    s.addressSearchComposing = true;
    WndProc(s.hwnd, WM_NULL, 0, 0);
    check(s.ui_schedule.deadline == s.addressHistoryDue, "IME composition removes the live-search wakeup");
    RunUiDeadlines(s, wall_now + 200);
    check(s.addressLiveDue != 0, "overdue live search stays suppressed during IME composition");
    s.addressSearchComposing = false;
    RunUiDeadlines(s, wall_now + 200);
    check(!s.addressLiveDue && s.addressHistoryDue, "stale debounce is consumed without submitting a new search");
    RunUiDeadlines(s, wall_now + 500);
    WndProc(s.hwnd, WM_NULL, 0, 0);
    check(!s.addressHistoryDue && !s.ui_schedule.deadline, "consumed deadlines cancel the shared timer");
    s.addressLiveDue = GetTickCount64();
    s.contentStatusTick = s.addressLiveDue;
    TickAddressSearchMaintenance(s, s.addressLiveDue);
    check(s.addressLiveDue != 0, "maintenance does not execute the separately scheduled debounce");
    TickAddressSearch(s, s.addressLiveDue);
    check(!s.addressLiveDue, "combined business tick retains deadline behavior for isolated callers");
    s.addressLiveDue = GetTickCount64() + 30;
    SyncUiScheduling(s);
    bool timer_delivered = false;
    const uint64_t timer_limit = GetTickCount64() + 500;
    while (GetTickCount64() < timer_limit) {
        MSG message{};
        if (PeekMessageW(&message, s.hwnd, WM_TIMER, WM_TIMER, PM_REMOVE) &&
            message.wParam == kTimerUiDeadline) {
            WndProc(s.hwnd, message.message, message.wParam, message.lParam);
            timer_delivered = true;
            break;
        }
        Sleep(1);
    }
    check(timer_delivered && !s.addressLiveDue && !s.ui_schedule.deadline,
          "real deadline timer delivers and consumes stale work without maintenance");
    s.renameClickCandidate = true;
    s.renameClickDue = GetTickCount64();
    s.renameClickPane = nullptr;
    RunUiDeadlines(s, s.renameClickDue);
    check(!s.renameClickCandidate && s.renameIndex < 0,
          "delayed rename rejects a changed pane before opening an editor");
    s.addressSearching = true;
    s.addressLiveDue = wall_now + 1000;
    const auto animation_now = app::PaneAnimationTimeMs();
    s.addressAnimationTick = animation_now;
    TickAddressSearchAnimation(s, animation_now + 20);
    check(s.addressLiveDue == wall_now + 1000 && s.addressSearchAnimation > 0,
          "address animation advances without running search business logic");
    s.addressSearching = false;
    s.addressSearchAnimation = 0;
    s.addressLiveDue = 0;
    s.sidebarScrollbarFade.Tick(static_cast<double>(animation_now), 40, false, false);
    s.sidebarScrollbarFade.Tick(static_cast<double>(animation_now + 70), 40, false, false);
    check(!s.sidebarScrollbarFade.Pending() && s.sidebarScrollbarFade.NextWake() > animation_now + 70,
          "scrollbar dwell waits for a deadline rather than continuous animation frames");
    s.sidebarScrollbarFade = {};
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.searchOptionsPending = true;
    SyncUiScheduling(s);
    const auto options_due = s.ui_schedule.deadline;
    Sleep(15);
    SyncUiScheduling(s);
    check(s.ui_schedule.maintenance_ms == 100 && options_due &&
          s.ui_schedule.deadline == options_due,
          "visible maintenance is 100ms and repeated input cannot postpone ready options");
    s.searchOptionsPending = false;
    s.sidebarScroll = 40;
    TickVisualAnimations(s);
    const auto fade_due = s.sidebarFadeDeadline;
    Sleep(2);
    TickVisualAnimations(s);
    check(fade_due > GetTickCount64() && fade_due == s.sidebarFadeDeadline,
          "scrollbar hold keeps a stable deadline across rendered frames");
    s.sidebarFadeDeadline = GetTickCount64();
    check(RunUiDeadlines(s, GetTickCount64()) && !s.sidebarFadeDeadline,
          "expired visual wake is consumed once even before a paint is delivered");
    ShowWindow(s.hwnd, SW_HIDE);
    s.sidebarScrollbarFade = {};
    s.sidebarScroll = 0;
    StopUiScheduling(s);
    SyncUiScheduling(s);
    check(!s.ui_schedule.deadline && !s.ui_schedule.maintenance_ms,
          "teardown cancels both channels and nested messages cannot rearm them");

    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
        s.scale = scale;
        s.darkMode = dark;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        const int width = dark ? 760 : 440;
        SetWindowPos(s.hwnd, nullptr, 0, 0, static_cast<int>(width * scale),
            static_cast<int>(540 * scale), SWP_NOACTIVATE | SWP_NOZORDER);
        s.filterEditing = true;
        s.pane->filter_animation_target = 1;
        s.pane->filter_animation_from = 0;
        s.pane->filter_animation_start = app::PaneAnimationTimeMs() - 45;
        Render(s);
        const auto output = root / ((dark ? std::wstring(L"dark-") : std::wstring(L"light-")) +
            std::to_wstring(static_cast<int>(scale * 100)) + L".png");
        check(s.compositor.SaveSnapshot(output.c_str()), "capture intermediate frame across themes, DPI and widths");
    }
    s.filterEditing = false;
    s.watches.Stop();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
