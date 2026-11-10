// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace pulse::ui {

inline UINT EditMotionFrameMessage() {
    static const UINT message = RegisterWindowMessageW(L"Pulse.EditMotionFrame");
    return message;
}

// One focused editor per renderer. The callback only posts; all drawing stays on the UI thread.
class EditFrameScheduler {
public:
    EditFrameScheduler() = default;
    EditFrameScheduler(const EditFrameScheduler&) = delete;
    EditFrameScheduler& operator=(const EditFrameScheduler&) = delete;
    ~EditFrameScheduler() {
        Cancel();
        if (wait_) CloseThreadpoolWait(wait_);
        if (timer_) CloseHandle(timer_);
    }

    bool Request(HWND hwnd) {
#ifdef PULSE_TEST_EDIT_VIEWPORT
        if (GetPropW(hwnd, L"Pulse.Test.EditWindowTimer")) { Cancel(hwnd); return false; }
#endif
        if (!initialized_) {
            initialized_ = true;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (timer_ && EditMotionFrameMessage()) wait_ = CreateThreadpoolWait(OnReady, this, nullptr);
        }
        // Keep the existing window timer available on systems without high-resolution timers.
        if (!wait_) return false;
        const auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        const auto now = Clock::now();
        if (hwnd_ != hwnd || monitor_ != monitor) {
            Cancel();
            hwnd_ = hwnd;
            monitor_ = monitor;
            token_ = next_token_.fetch_add(1, std::memory_order_relaxed);
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            DEVMODEW mode{}; mode.dmSize = sizeof(mode);
            const bool queried = GetMonitorInfoW(monitor, &info) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode);
            const DWORD refresh = queried && mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 60;
            interval_ = std::chrono::nanoseconds(1'000'000'000 / refresh);
            deadline_ = now + interval_;
        }
        // Repaints and retargeting must not move an already requested frame's deadline.
        if (pending_) return true;
        if (deadline_ <= now) deadline_ += interval_ * ((now - deadline_) / interval_ + 1);
        using TimerTicks = std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>;
        LARGE_INTEGER due{};
        due.QuadPart = -(std::max)(LONGLONG{1}, std::chrono::duration_cast<TimerTicks>(deadline_ - now).count());
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) return false;
        pending_ = true;
        SetThreadpoolWait(wait_, timer_, nullptr);
        return true;
    }

    bool Consume(HWND hwnd, WPARAM token) {
        if (!pending_ || hwnd != hwnd_ || token != token_) return false;
        pending_ = false;
        deadline_ += interval_;
        return true;
    }

    void Cancel(HWND hwnd = nullptr) {
        if (!hwnd_ || (hwnd && hwnd != hwnd_)) return;
        SetThreadpoolWait(wait_, nullptr, nullptr);
        CancelWaitableTimer(timer_);
        // A callback may already be posting; join it before changing its HWND/token or releasing storage.
        WaitForThreadpoolWaitCallbacks(wait_, TRUE);
        hwnd_ = nullptr;
        monitor_ = nullptr;
        pending_ = false;
    }

private:
    static void CALLBACK OnReady(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) {
        auto& self = *static_cast<EditFrameScheduler*>(context);
        if (!PostMessageW(self.hwnd_, EditMotionFrameMessage(), self.token_, 0)) self.pending_ = false;
    }
    using Clock = std::chrono::steady_clock;
    inline static std::atomic<UINT_PTR> next_token_{1};
    HANDLE timer_ = nullptr;
    PTP_WAIT wait_ = nullptr;
    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    UINT_PTR token_ = 0;
    bool initialized_ = false;
    std::atomic<bool> pending_{false};
    Clock::duration interval_{};
    Clock::time_point deadline_{};
};

} // namespace pulse::ui
