// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#pragma once
#include "../ui/edit_motion.h"
#include "../ui/edit_frame_scheduler.h"
#include <algorithm>
#include <chrono>
#include <vector>
#include <psapi.h>

namespace {
using EditProfileClock = std::chrono::steady_clock;
struct EditProfileResources {
    uint64_t cpu = 0;
    SIZE_T working = 0, committed = 0;
    DWORD handles = 0;
    static EditProfileResources Read() {
        EditProfileResources value;
        FILETIME created{}, exited{}, kernel{}, user{};
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) &&
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) &&
            GetProcessHandleCount(GetCurrentProcess(), &value.handles), "sample isolated editor process resources");
        value.cpu = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
            (static_cast<uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
        value.working = memory.WorkingSetSize; value.committed = memory.PrivateUsage;
        return value;
    }
};
struct EditFrameProfile {
    std::vector<double> frames, paints;
    bool active = false;
    uint64_t motion_messages = 0, caret_timers = 0;
};
LRESULT CALLBACK ProfileEditFrames(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& profile = *reinterpret_cast<EditFrameProfile*>(data);
    if (message == pulse::ui::EditMotionFrameMessage() ||
        (message == WM_TIMER && wp == pulse::ui::kEditMotionTimer)) ++profile.motion_messages;
    if (message == WM_TIMER && wp == pulse::ui::kEditCaretTimer) ++profile.caret_timers;
    const auto before = EditProfileClock::now();
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (profile.active && message == WM_PAINT) {
        const auto after = EditProfileClock::now();
        profile.frames.push_back(std::chrono::duration<double, std::milli>(before.time_since_epoch()).count());
        profile.paints.push_back(std::chrono::duration<double, std::milli>(after - before).count());
    }
    return result;
}
double EditProfilePercentile(std::vector<double> values, double percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>((values.size() - 1) * percentile)];
}
bool AwaitEditFrame(HWND edit, MSG& frame) {
    const auto until = EditProfileClock::now() + std::chrono::milliseconds(1000);
    while (EditProfileClock::now() < until) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.hwnd == edit && message.message == pulse::ui::EditMotionFrameMessage()) {
                frame = message;
                return true;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return false;
}
void PumpEditProfile(unsigned milliseconds) {
    const auto until = EditProfileClock::now() + std::chrono::milliseconds(milliseconds);
    while (EditProfileClock::now() < until) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(until - EditProfileClock::now()).count();
        if (remaining > 0) MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(remaining), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
}

inline void RunEditFrameProfile(HWND parent, bool foreground_profile = false) {
    using namespace pulse::ui;
    Compositor compositor;
    Check(parent && compositor.Init(parent), "initialize isolated input frame profiler");
    compositor.RecreateTextFormats(1.0f);
    HWND edit = CreateChildEdit(parent, L"Continuous caret movement and text input");
    Check(edit && compositor.CustomEditEnabled(), "profile the production custom editor");
    if (!edit || !compositor.CustomEditEnabled()) { if (edit) DestroyWindow(edit); return; }
    SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&compositor));
    SetPropW(edit, L"Pulse.Test.EditMotion", reinterpret_cast<HANDLE>(1));
    SetWindowPos(edit, nullptr, 20, 30, 420, 32, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(parent, foreground_profile ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
    if (foreground_profile) SetForegroundWindow(parent);
    SetFocus(edit);
    Check(GetFocus() == edit, "resource profiler retains real native editor focus");
    EditFrameProfile profile;
    SetWindowSubclass(edit, ProfileEditFrames, 2, reinterpret_cast<DWORD_PTR>(&profile));
    MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    GetMonitorInfoW(MonitorFromWindow(edit, MONITOR_DEFAULTTONEAREST), &monitor);
    EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode);
    BYTE saved_keys[256]{}, keys[256]{};
    const bool have_keys = GetKeyboardState(saved_keys) != FALSE;
    Check(have_keys, "save thread modifiers for selection resource profile");
    for (const int scenario : {0, 1, 2}) {
        const bool typing = scenario == 1;
        const char* name = typing ? "typing" : scenario == 2 ? "selection" : "caret";
        keys[VK_SHIFT] = scenario == 2 ? 0x80 : 0;
        if (have_keys) Check(SetKeyboardState(keys) != FALSE, "set isolated profile modifiers");
        SetWindowTextW(edit, typing ? L"" : L"Continuous caret movement and text input");
        SendMessageW(edit, EM_SETSEL, 0, 0);
        PumpEditProfile(120);
        profile = {};
        profile.active = true;
        const HWND foreground_context = GetForegroundWindow();
        DWORD foreground_process = 0;
        GetWindowThreadProcessId(foreground_context, &foreground_process);
        const bool foreground_self = foreground_process == GetCurrentProcessId();
        const auto resources_before = EditProfileResources::Read();
        const auto stats_before = *compositor.GetLumaTextStats();
        const auto started = EditProfileClock::now();
        auto next_input = started;
        unsigned inputs = 0;
        bool selection_inputs_valid = true;
        while (EditProfileClock::now() - started < std::chrono::milliseconds(4000)) {
            if (!IsWindow(edit)) {
                if (have_keys) SetKeyboardState(saved_keys);
                Check(false, "resource measurement cancelled by closing its window");
                return;
            }
            const auto now = EditProfileClock::now();
            if (now >= next_input) {
                if (typing) SendMessageW(edit, WM_CHAR, inputs % 2 ? VK_BACK : L'x', 0);
                else if (scenario == 2) {
                    // Message pumping may refresh the thread's physical modifier state.
                    selection_inputs_valid &= SetKeyboardState(keys) != FALSE;
                    SendMessageW(edit, WM_KEYDOWN, inputs % 2 ? VK_LEFT : VK_RIGHT, 0);
                    DWORD first = 0, last = 0;
                    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
                    const bool selected = first == 0 && last == (inputs % 2 ? 0u : 1u);
                    if (!selected && inputs < 4) std::printf("[DIAG] selection input=%u shift=%x range=%lu,%lu\n",
                        inputs, static_cast<unsigned>(GetKeyState(VK_SHIFT)), first, last);
                    selection_inputs_valid &= selected;
                }
                else {
                    const auto position = inputs % 2 ? 0 : GetWindowTextLengthW(edit);
                    SendMessageW(edit, EM_SETSEL, position, position);
                }
                ++inputs;
                next_input = now + std::chrono::milliseconds(40);
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(next_input - EditProfileClock::now()).count();
            if (remaining > 0) MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(remaining), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        profile.active = false;
        std::vector<double> intervals;
        for (size_t i = 1; i < profile.frames.size(); ++i) intervals.push_back(profile.frames[i] - profile.frames[i - 1]);
        std::printf("[METRIC] %s screen_hz=%lu inputs=%u scheduled_frames=%zu interval_median_ms=%.3f interval_p95_ms=%.3f paint_p95_ms=%.3f\n",
            name, mode.dmDisplayFrequency, inputs, profile.frames.size(),
            EditProfilePercentile(intervals, 0.5), EditProfilePercentile(intervals, 0.95),
            EditProfilePercentile(profile.paints, 0.95));
        if (foreground_profile) {
            if (GetForegroundWindow() != foreground_context || GetFocus() != edit) {
                DWORD owner = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &owner);
                std::printf("[DIAG] foreground=%p owner=%lu parent=%p focus=%p edit=%p self=%lu\n",
                    GetForegroundWindow(), owner, parent, GetFocus(), edit, GetCurrentProcessId());
            }
            Check(GetForegroundWindow() == foreground_context && GetFocus() == edit,
                "measurement retains native focus and a stable foreground context");
        }
        const auto resources_after = EditProfileResources::Read();
        const auto& stats_after = *compositor.GetLumaTextStats();
        std::printf("[METRIC] resources scenario=%s wall_ms=%.3f cpu_ms=%.3f working_kib=%zu private_kib=%zu handles=%lu glyph_draws=%llu layout_builds=%llu paints=%llu foreground_self=%d\n",
            name, std::chrono::duration<double, std::milli>(EditProfileClock::now() - started).count(),
            static_cast<double>(resources_after.cpu - resources_before.cpu) / 10000,
            resources_after.working / 1024, resources_after.committed / 1024, resources_after.handles,
            static_cast<unsigned long long>(stats_after.edit_lumatext_draws - stats_before.edit_lumatext_draws),
            static_cast<unsigned long long>(stats_after.edit_layout_builds - stats_before.edit_layout_builds),
            static_cast<unsigned long long>(stats_after.edit_directwrite_draws - stats_before.edit_directwrite_draws), foreground_self);
        if (scenario == 2) Check(selection_inputs_valid,
            "selection workload changes the actual native range on every synthetic key");
        Check(profile.frames.size() >= 12 && !GetPropW(edit, L"Pulse.NativeEditFallback"),
            "frame profile receives scheduled paints without native fallback");
        PumpEditProfile(180);
        Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "profiled motion settles and releases frame requests");
    }
    if (have_keys) Check(SetKeyboardState(saved_keys) != FALSE, "restore profile thread modifiers");
    for (const int state : {0, 1, 2}) {
        if (state == 1) SetFocus(nullptr);
        if (state == 2) ShowWindow(edit, SW_HIDE);
        const auto before = EditProfileResources::Read();
        const auto paints = compositor.GetLumaTextStats()->edit_directwrite_draws;
        const auto motion_messages = profile.motion_messages, caret_timers = profile.caret_timers;
        PumpEditProfile(2000);
        const auto after = EditProfileResources::Read();
        const auto draws = compositor.GetLumaTextStats()->edit_directwrite_draws - paints;
        std::printf("[METRIC] idle state=%s cpu_ms=%.3f working_kib=%zu private_kib=%zu handles=%lu paints=%llu\n",
            state == 0 ? "focused" : state == 1 ? "unfocused" : "hidden", static_cast<double>(after.cpu - before.cpu) / 10000,
            after.working / 1024, after.committed / 1024, after.handles, static_cast<unsigned long long>(draws));
        std::printf("[METRIC] idle_timers state=%d blink_ms=%u caret_timers=%llu motion_messages=%llu\n", state,
            GetCaretBlinkTime(), static_cast<unsigned long long>(profile.caret_timers - caret_timers),
            static_cast<unsigned long long>(profile.motion_messages - motion_messages));
        // Native blinking and invalidation are not high-frequency animation requests.
        Check(!GetPropW(edit, L"Pulse.EditMotionPending") && profile.motion_messages == motion_messages &&
            (state == 0 || draws <= 1), "idle editors stop high-frequency paint work");
    }
    ShowWindow(edit, SW_SHOWNOACTIVATE);
    SetFocus(edit);
    SetWindowTextW(edit, L"frames");
    SendMessageW(edit, EM_SETSEL, 0, 0);
    PumpEditProfile(180);
    Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "queue fixture settles previous text and caret changes");
    SendMessageW(edit, EM_SETSEL, 6, 6);
    if (reinterpret_cast<UINT_PTR>(GetPropW(edit, L"Pulse.EditMotionPending")) == 1) {
        MSG queued{};
        const bool received = AwaitEditFrame(edit, queued);
        Check(received, "high-resolution frame reaches the editor message queue");
        if (received) {
            Sleep(40);
            MSG extra{};
            Check(!PeekMessageW(&extra, edit, EditMotionFrameMessage(), EditMotionFrameMessage(), PM_NOREMOVE),
                "a blocked UI accumulates at most one scheduled motion frame");
            SetFocus(nullptr);
            Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "focus loss cancels queued motion");
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, 0);
            SendMessageW(edit, EM_SETSEL, 6, 6);
            const auto before_stale = compositor.GetLumaTextStats()->edit_directwrite_draws;
            DispatchMessageW(&queued);
            Check(compositor.GetLumaTextStats()->edit_directwrite_draws == before_stale &&
                GetPropW(edit, L"Pulse.EditMotionPending"), "old queued frame cannot consume a newly focused motion session");
        }
    }
    PumpEditProfile(160);
    const auto idle_draws = compositor.GetLumaTextStats()->edit_directwrite_draws;
    PumpEditProfile(80);
    Check(!GetPropW(edit, L"Pulse.EditMotionPending") &&
        compositor.GetLumaTextStats()->edit_directwrite_draws == idle_draws,
        "settled editor stops high-frequency wakeups before the next blink");
    SetPropW(edit, L"Pulse.Test.EditWindowTimer", reinterpret_cast<HANDLE>(1));
    SendMessageW(edit, EM_SETSEL, 0, 0);
    Check(reinterpret_cast<UINT_PTR>(GetPropW(edit, L"Pulse.EditMotionPending")) == 2,
        "unavailable high-resolution timer retains the window timer fallback");
    PumpEditProfile(180);
    Check(!GetPropW(edit, L"Pulse.EditMotionPending"), "fallback animation also settles and stops");
    RemovePropW(edit, L"Pulse.Test.EditWindowTimer");
    SendMessageW(edit, EM_SETSEL, 6, 6);
    DestroyWindow(edit);
    PumpEditProfile(40);
    Check(!IsWindow(edit), "destroying an animated editor safely cancels its callback");
}
