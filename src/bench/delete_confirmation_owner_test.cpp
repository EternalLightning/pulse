// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "owned_test_window.h"
#include <cstdio>
#include <thread>

namespace {
struct Window {
    HWND hwnd = nullptr;
    ~Window() { if (hwnd) DestroyWindow(hwnd); }
};
}

int main() {
    using namespace pulse::bench;
    setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr wchar_t name[] = L"PulseConfirmWindow";
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = name;
    if (!RegisterClassW(&type)) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    const auto create = [&](const wchar_t* class_name, HWND owner) {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"Owned confirmation fixture",
            WS_POPUP | WS_VISIBLE, -10000, -10000, 10, 10, owner, nullptr, type.hInstance, nullptr);
    };
    Window owner{create(L"STATIC", nullptr)}, other_owner{create(L"STATIC", nullptr)};
    Window intended{create(name, owner.hwnd)}, decoy{create(name, other_owner.hwnd)};
    Window wrong_class{create(L"STATIC", owner.hwnd)};
    check(owner.hwnd && other_owner.hwnd && intended.hwnd && decoy.hwnd && wrong_class.hwnd,
        "fixture creates only its own owner, confirmation and decoy windows");
    check(IsOwnedThreadWindow(intended.hwnd, owner.hwnd, name), "intended confirmation is admitted");
    check(!IsOwnedThreadWindow(decoy.hwnd, owner.hwnd, name), "same-class confirmation with another owner is rejected");
    check(!IsOwnedThreadWindow(wrong_class.hwnd, owner.hwnd, name), "another class with the expected owner is rejected");
    check(FindOwnedThreadWindow(owner.hwnd, name) == intended.hwnd,
        "discovery selects the intended owner while the same-class decoy stays untouched");
    ShowWindow(intended.hwnd, SW_HIDE);
    check(!IsOwnedThreadWindow(intended.hwnd, owner.hwnd, name) && !FindOwnedThreadWindow(owner.hwnd, name),
        "hidden confirmations are excluded");
    ShowWindow(intended.hwnd, SW_SHOWNOACTIVATE);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready || !stop || !done) {
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
        if (done) CloseHandle(done);
        return 2;
    }
    const auto wait = [](HANDLE event, DWORD timeout) {
        const ULONGLONG deadline = GetTickCount64() + timeout;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            const DWORD result = MsgWaitForMultipleObjects(1, &event, FALSE,
                static_cast<DWORD>(deadline - now), QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) return true;
            if (result != WAIT_OBJECT_0 + 1) return false;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    };
    HWND foreign_thread = nullptr;
    std::thread worker([&] {
        {
            Window window{create(name, owner.hwnd)};
            foreign_thread = window.hwnd;
            SetEvent(ready);
            WaitForSingleObject(stop, 10000);
        }
        SetEvent(done);
    });
    const bool ready_result = wait(ready, 5000);
    check(ready_result && foreign_thread &&
        !IsOwnedThreadWindow(foreign_thread, owner.hwnd, name),
        "another UI thread is excluded even with the same process, class and owner");
    check(FindOwnedThreadWindow(owner.hwnd, name) == intended.hwnd,
        "discovery stays on the fixture UI thread");
    SetEvent(stop);
    check(wait(done, 5000), "fixture UI thread destroys its own window and exits");
    worker.join();
    CloseHandle(ready);
    CloseHandle(stop);
    CloseHandle(done);
    return failures ? 1 : 0;
}
