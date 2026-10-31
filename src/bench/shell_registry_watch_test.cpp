// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/shell_registry_watch_context.h"
#include <cstdio>
#include <future>
#include <thread>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
struct Worker {
    std::unique_ptr<pulse::app::ShellRegistryWatchContext> context;
    HANDLE gate;
    std::promise<DWORD> result;
};
DWORD WINAPI Delayed(LPVOID raw) {
    std::unique_ptr<Worker> worker(static_cast<Worker*>(raw));
    WaitForSingleObject(worker->gate, INFINITE);
    worker->result.set_value(WaitForSingleObject(worker->context->stop, 0));
    return 0;
}
}

int main() {
    using namespace pulse::app;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto context = std::make_unique<ShellRegistryWatchContext>();
    Check(stop && gate && context->DuplicateStop(stop), "real stop handle duplicated for worker");
    if (!stop || !gate || !context->stop) return 1;
    auto worker = std::make_unique<Worker>(Worker{std::move(context), gate, {}});
    auto observed = worker->result.get_future();
    HANDLE thread = CreateThread(nullptr, 0, Delayed, worker.get(), 0, nullptr);
    Check(thread != nullptr, "delayed worker created");
    if (!thread) { CloseHandle(stop); CloseHandle(gate); return 1; }
    worker.release();
    HANDLE completion = nullptr;
    DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &completion,
        SYNCHRONIZE, FALSE, 0);
    StopShellRegistryWatchHandles(stop, thread, 0);
    Check(!stop && !thread && observed.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
        "bounded stop closes UI handles while worker remains gated");
    SetEvent(gate);
    Check(observed.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
        observed.get() == WAIT_OBJECT_0, "late worker still observes signaled owned stop handle");
    if (completion) { WaitForSingleObject(completion, 5000); CloseHandle(completion); }
    CloseHandle(gate);

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"registry watch fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, "message-only window created");
    if (!hwnd) return 1;
    constexpr UINT message = WM_APP + 49;
    auto previous = std::make_shared<WindowResultMailbox<bool>>(hwnd, message);
    previous->Publish(true);
    previous->Close();
    MSG stale{};
    PeekMessageW(&stale, hwnd, message, message, PM_REMOVE);
    WindowResultMailbox<bool> current(hwnd, message);
    current.Publish(true);
    Check(stale.message == message && current.Take(stale.wParam).empty(),
        "old registry notification cannot invalidate replacement window session");
    MSG next{};
    PeekMessageW(&next, hwnd, message, message, PM_REMOVE);
    Check(current.Take(next.wParam).size() == 1 && !previous->Publish(true),
        "replacement notification survives stale message; closed producer rejects publish");
    std::promise<void> late_release;
    auto late_gate = late_release.get_future();
    bool posted = true;
    std::thread late([previous, &posted, late_gate = std::move(late_gate)]() mutable {
        late_gate.wait();
        posted = previous->Publish(true);
    });
    late_release.set_value();
    late.join();
    Check(!posted, "late registry worker completion after close posts nothing");
    current.Close();
    DestroyWindow(hwnd);
    return failures ? 1 : 0;
}
