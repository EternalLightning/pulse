#pragma once
#include "../ops/clipboard.h"
#include <windows.h>
#include <ole2.h>
#include <cstdio>

namespace pulse::ops {

inline int RunClipboardIsolationRegressions() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    // Clipboard contents belong to a window station. Never empty the user's
    // interactive clipboard, including when isolation cannot be established.
    const auto previous_station = GetProcessWindowStation();
    const auto previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    // Medium-integrity callers must let Windows name the station. Creation
    // only prevents reopening an existing station and touching its clipboard.
    const auto station = CreateWindowStationW(nullptr, CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
    const auto create_error = station ? ERROR_SUCCESS : GetLastError();
    const bool attached = station && SetProcessWindowStation(station);
    const auto attach_error = station && !attached ? GetLastError() : ERROR_SUCCESS;
    if (!attached) {
        std::printf("[INFO] CreateWindowStation=%lu SetProcessWindowStation=%lu\n", create_error, attach_error);
        if (station) CloseWindowStation(station);
        if (create_error == ERROR_ALREADY_EXISTS || create_error == ERROR_ACCESS_DENIED ||
            attach_error == ERROR_ACCESS_DENIED) {
            std::printf("[SKIP] no fresh private clipboard station is available; "
                        "0 assertions executed; user clipboard untouched\n");
            return 77;
        }
        check(false, "create a private clipboard window station");
        return 1;
    }
    check(true, "create a private clipboard window station");
    const auto desktop = CreateDesktopW(L"ClipboardFixture", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool switched = desktop && SetThreadDesktop(desktop);
    check(switched, "attach the fixture thread to its private desktop");
    HWND owner = nullptr;
    if (switched) {
        owner = CreateWindowExW(0, L"STATIC", L"Pulse clipboard fixture", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
        if (owner) SetActiveWindow(owner);
        check(owner && GetActiveWindow() == owner, "clipboard owner belongs to the private station");
        if (owner && GetActiveWindow() == owner) {
            const std::vector<std::wstring> paths{L"Z:\\Pulse fixture\\中文.txt", L"Z:\\Pulse fixture\\empty.txt"};
            ClipboardData data;
            check(WriteClipboard(paths, false) && ReadClipboard(data) && data.paths == paths && !data.cut,
                "copy round-trips multiple Unicode network paths through CF_HDROP");
            check(WriteClipboard(paths, true) && ReadClipboard(data) && data.paths == paths && data.cut && data.sequence,
                "cut round-trips network paths and an exact move preference");
            const auto cut = data;
            check(!CompleteCutClipboard(cut.sequence, {paths.front()}) && ReadClipboard(data) && data.cut,
                "incomplete cut completion preserves the captured clipboard");
            check(CompleteCutClipboard(cut.sequence, cut.paths) && !ReadClipboard(data),
                "successful cut completion clears its own captured file list");
            check(WriteClipboard(paths, true) && ReadClipboard(data), "capture another cut before replacement");
            const auto stale = data;
            check(WriteClipboard(paths, false) && !CompleteCutClipboard(stale.sequence, stale.paths) &&
                ReadClipboard(data) && data.paths == paths && !data.cut,
                "a late cut completion never clears a newer copy");
            check(!PreferredEffectIsCut(DROPEFFECT_COPY | DROPEFFECT_MOVE) && PreferredEffectIsCut(DROPEFFECT_MOVE),
                "only an unambiguous move effect authorizes cutting");
        }
    }
    if (owner) DestroyWindow(owner);
    check(SetProcessWindowStation(previous_station) != FALSE, "restore the original process window station");
    check(SetThreadDesktop(previous_desktop) != FALSE, "restore the original thread desktop");
    if (desktop) CloseDesktop(desktop);
    CloseWindowStation(station);
    return failures ? 1 : 0;
}

} // namespace pulse::ops
