#include "../app/ui_schedule_state.h"
#include "../app/ui_change_signal.h"
#include <cstdio>
#include <thread>

int main() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    UiScheduleState schedule;
    schedule.NoteActivity(1000);
    check(schedule.MaintenanceInterval(true, false, 5999) == 100 &&
        schedule.MaintenanceInterval(true, false, 6000) == 500,
        "quiet polling begins only after five seconds without activity");
    schedule.NoteActivity(6100);
    check(schedule.MaintenanceInterval(true, false, 6100) == 100,
        "new input or background results immediately restore active polling");
    check(schedule.MaintenanceInterval(false, false, 6100) == 1000 &&
        schedule.MaintenanceInterval(false, true, 6100) == 200,
        "hidden polling slows while shutdown keeps its bounded fallback");
    UiDeadlinePlan deadline;
    deadline.Include(6120);
    check(deadline.Delay(6100) == 20,
        "independent deadlines stay precise while maintenance polling slows");

    constexpr UINT wake = WM_APP + 1;
    const HWND hwnd = CreateWindowExW(0, L"STATIC", L"UI change signal fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(hwnd != nullptr, "create a real message target for producer wakeups");
    if (!hwnd) return 2;
    const auto take_messages = [&] {
        unsigned count = 0;
        MSG message{};
        while (PeekMessageW(&message, hwnd, wake, wake, PM_REMOVE)) ++count;
        return count;
    };
    UiChangeSignal changed;
    changed = true;
    changed.SetTarget(hwnd, wake);
    check(take_messages() == 1 && changed.Take() && !changed.Take(),
        "a result produced before target registration wakes the newly attached UI");
    std::thread producer([&] {
        for (unsigned i = 0; i < 100; ++i) changed = true;
    });
    producer.join();
    check(take_messages() == 1 && changed.Take(),
        "one hundred background changes coalesce to one wake until consumed");
    std::thread next_batch([&] { changed = true; });
    next_batch.join();
    check(take_messages() == 1 && changed.Take(),
        "consuming a batch re-arms the next producer wakeup");
    changed.SetTarget(nullptr, 0);
    std::thread stopped_producer([&] { changed = true; });
    stopped_producer.join();
    check(take_messages() == 0 && changed.Take(),
        "disconnect prevents late producers from posting while preserving pending data");
    DestroyWindow(hwnd);
    std::printf("STARTUP IDLE POLICY failures=%d\n", failures);
    return failures ? 1 : 0;
}
