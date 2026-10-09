#include "../ui/playback_timeline.h"
#include "../ui/video_preview.h"
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <atomic>

using pulse::ui::VideoPreview;
static int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!ok) ++failures;
}
void Pump(unsigned ms) {
    const auto end = GetTickCount64() + ms;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() < end);
}
bool Until(const std::function<bool()>& condition, unsigned ms = 10000) {
    const auto end = GetTickCount64() + ms;
    do { Pump(20); if (condition()) return true; } while (GetTickCount64() < end);
    return false;
}
#ifdef PULSE_VIDEO_PREVIEW_TESTING
int AdmissionCases() {
    struct Gate {
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<uint32_t> entered{0};
        ~Gate() { if (release) CloseHandle(release); }
    };
    HWND window = CreateWindowExW(0, L"STATIC", L"Video admission fixture", 0,
        0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto gate = std::make_shared<Gate>();
    Check(window && gate->release, "create isolated video worker gates and owner");
    if (!window || !gate->release) { if (window) DestroyWindow(window); return 1; }
    VideoPreview::SetWorkerHookForTest([gate] {
        ++gate->entered;
        WaitForSingleObject(gate->release, INFINITE);
    });
    VideoPreview video;
    const uint32_t limit = VideoPreview::ResourcesForTest().limit;
    for (uint32_t i = 0; i < limit; ++i) {
        video.Open(window, L"blocked-provider-fixture.mp4");
        Check(Until([&] { return gate->entered.load() == i + 1; }, 3000),
              "native video worker enters blocked provider gate");
        video.Reset();
    }
    auto usage = VideoPreview::ResourcesForTest();
    Check(usage.live == limit && usage.retired == limit,
          "Reset retains all four blocked native workers and retired owners");
    bool rejected = true;
    for (int i = 0; i < 32; ++i) {
        video.Open(window, L"overflow-provider-fixture.mp4");
        rejected &= video.Snapshot().error == HRESULT_FROM_WIN32(ERROR_BUSY);
        video.Reset();
    }
    int children = 0;
    EnumChildWindows(window, [](HWND, LPARAM data) -> BOOL {
        ++*reinterpret_cast<int*>(data); return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    Check(rejected && gate->entered.load() == limit && children == static_cast<int>(limit) &&
          VideoPreview::ResourcesForTest().live == limit,
          "rapid Open cannot add threads or hidden render targets beyond physical admission");
    SetEvent(gate->release);
    Check(Until([] { return VideoPreview::ResourcesForTest().live == 0; }, 3000),
          "slots release only after blocked native worker threads actually exit");
    children = 0;
    EnumChildWindows(window, [](HWND, LPARAM data) -> BOOL {
        ++*reinterpret_cast<int*>(data); return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    Check(children == 0, "retirement messages clean up completed render targets");
    VideoPreview::SetWorkerHookForTest([] {});
    video.Open(window, L"reusable-provider-fixture.mp4");
    Check(SUCCEEDED(video.Snapshot().error) &&
          Until([] { return VideoPreview::ResourcesForTest().live == 0; }, 3000),
          "completed physical workers permit the next open");
    video.Reset();
    Pump(20);
    VideoPreview::SetWorkerHookForTest({});
    DestroyWindow(window);
    return failures ? 1 : 0;
}
#endif
int wmain(int argc, wchar_t** argv) {
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    if (argc == 2 && wcscmp(argv[1], L"--admission-only") == 0) return AdmissionCases();
#endif
    const bool open_only = argc == 3 && wcscmp(argv[2], L"--open-only") == 0;
    const bool end_seek_only = argc == 3 && wcscmp(argv[2], L"--end-seek-only") == 0;
    using namespace pulse::ui::playback;
    if (!open_only && !end_seek_only) {
        Check(FrameAt(0, 20) == 0 && FrameAt(1, 20) == 19, "GIF seek endpoints are valid frame indices");
        Check(FrameAt(.5, 11) == 5, "GIF seek maps progress to frame");
        Check(FrameAt(-1, 10) == 0 && FrameAt(2, 10) == 9, "GIF seek clamps beyond track");
        Check(FrameAt(.5, 0) == 0 && FrameAt(1, 1) == 0, "empty/static timelines do not underflow");
        Check(FrameAt(std::numeric_limits<double>::quiet_NaN(), 10) == 0, "invalid seek is bounded");
        Check(StepFrame(0, -1, 8) == 0 && StepFrame(7, 1, 8) == 7, "manual steps stop at endpoints");
        Check(StepFrame(3, -1, 8) == 2 && StepFrame(3, 1, 8) == 4, "manual previous/next are one frame");
        Check(Fraction(-1, 0, 100) == 0 && Fraction(120, 0, 100) == 1, "drag clamps outside window");
        Check(Fraction(50, 0, 100) == .5 && Fraction(1, 1, 1) == 0, "DPI-independent fraction and zero width");
        Check(VideoPreview::Supports(L"test.MP4") && !VideoPreview::Supports(L"test.gif"), "video selection preserves GIF path");
    }
    if (argc == 2 && wcscmp(argv[1], L"--timeline-only") == 0) return failures ? 1 : 0;
    if (argc != 2 && !open_only && !end_seek_only) { std::puts("Video runtime tests require an isolated fixture path"); return 2; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse playback regression fixture", WS_OVERLAPPEDWINDOW,
        80, 80, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    VideoPreview video;
    video.Open(window, argv[1]);
    video.Layout(RECT{0, 0, 460, 260}, true);
    const bool opened = Until([&] { auto s = video.Snapshot(); return s.ready || FAILED(s.error); });
    auto state = video.Snapshot();
    std::printf("open HRESULT=%08lx ready=%d size=%ux%u duration=%lld\n",
        static_cast<unsigned long>(state.error), state.ready, state.width, state.height, state.duration);
    Check(opened && state.ready && SUCCEEDED(state.error), "real MP4 opens through production media worker");
    if (end_seek_only) {
        if (state.ready) {
            video.Play(false);
            Pump(500);
            video.Seek(1.0);
            const bool at_end = Until([&] {
                const auto s = video.Snapshot();
                return FAILED(s.error) || (!s.busy && s.position > s.duration * 9 / 10);
            }, 12000);
            state = video.Snapshot();
            std::printf("end error=%08lx control=%08lx busy=%d position=%lld duration=%lld\n",
                static_cast<unsigned long>(state.error), static_cast<unsigned long>(state.control_error),
                state.busy, state.position, state.duration);
            Check(at_end && SUCCEEDED(state.error), "seek to track endpoint remains usable");
            video.Seek(.25);
            Check(Until([&] {
                const auto s = video.Snapshot();
                return SUCCEEDED(s.error) && !s.busy &&
                    std::llabs(s.position - s.duration / 4) < 1500000;
            }, 4000), "seek backward from endpoint reaches quarter position");
        }
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (open_only) {
        if (state.ready) Check(Until([&] { return video.Snapshot().position > 2000000; }),
            "video clock advances while playing");
        video.Reset();
        Pump(800);
        DestroyWindow(window);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (state.ready) {
        Check(Until([&] { return video.Snapshot().position > 2000000; }), "video clock advances while playing");
        video.Play(false);
        Pump(400);
        auto before = video.Snapshot();
        Pump(400);
        auto after = video.Snapshot();
        Check(!after.playing && std::llabs(after.position - before.position) < 50000,
            "pause freezes real media clock");
        Check(after.can_seek && after.duration > 0, "seek capability and duration reported");
        video.Seek(.5);
        Check(Until([&] { auto s = video.Snapshot(); return !s.busy &&
            std::llabs(s.position - s.duration / 2) < 1500000; }), "paused seek reaches midpoint without auto-resume");
        before = video.Snapshot();
        video.Step();
        const bool stepped = Until([&] { auto s = video.Snapshot(); return s.stepped_frames == 1 || FAILED(s.control_error); });
        after = video.Snapshot();
        std::printf("step HRESULT=%08lx delta=%lld count=%u\n", static_cast<unsigned long>(after.control_error),
            after.position - before.position, after.stepped_frames);
        Check(stepped && after.stepped_frames == 1 && !after.playing && SUCCEEDED(after.control_error),
            "MFPlay confirms exact forward frame step while paused");
        // 10 fps fixture: verify timestamp advances by one frame, not arbitrary milliseconds.
        Check(std::llabs((after.position - before.position) - 1000000) < 200000,
            "single step advances fixture by one 100 ms frame");
        video.Step(); video.Step(); video.Step();
        Check(Until([&] { return video.Snapshot().stepped_frames == 4; }), "rapid step clicks are serialized");
        video.Seek(.1); video.Seek(.9); video.Seek(.25);
        Check(Until([&] { auto s = video.Snapshot(); return !s.busy &&
            std::llabs(s.position - s.duration / 4) < 1500000; }), "rapid seeks settle on newest request");
        video.Play(true);
        Check(Until([&] { return video.Snapshot().position > video.Snapshot().duration / 2; }), "resume after seek advances");
        Check(Until([&] { return video.Snapshot().ended; }), "end of media stops playback");
        video.Play(true);
        Check(Until([&] { auto s = video.Snapshot(); return s.playing && !s.ended && s.position < s.duration / 2; }),
            "play at EOF restarts from beginning");
    }
    video.Reset();
    Check(!video.active(), "close clears media state immediately");
    for (int i = 0; i < 4; ++i) { video.Open(window, argv[1]); Pump(30); video.Reset(); }
    video.Open(window, L"Z:\\__pulse_missing_fixture__\\missing.mp4");
    Check(Until([&] { return FAILED(video.Snapshot().error); }), "missing media reports failure instead of spinning");
    Check(!video.Snapshot().ready, "old video cannot overwrite newer failed open");
    video.Reset();
    Pump(800);
    int children = 0;
    EnumChildWindows(window, [](HWND, LPARAM data) -> BOOL {
        ++*reinterpret_cast<int*>(data); return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    Check(children == 0, "retired video render windows are released after rapid close");
    DestroyWindow(window);
    Pump(200);
    CoUninitialize();
    std::printf("Failures: %d\n", failures);
    return failures ? 1 : 0;
}
