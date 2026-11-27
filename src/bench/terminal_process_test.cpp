// SPDX-License-Identifier: Apache-2.0
#include "../ops/terminal_process.h"
#include <cstdio>

namespace {
using pulse::ops::TerminalProcess;
using pulse::ops::TerminalShell;
std::string output;
HWND owner = nullptr;
TerminalProcess terminal;
TerminalProcess::Status status = TerminalProcess::Status::Starting;

LRESULT CALLBACK Receive(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == pulse::ops::kTerminalUpdated) {
        if (window != owner || !wp || wp != terminal.NotificationId()) return 0;
        auto update = terminal.TakeOutput();
        output += update.output;
        status = update.status;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

bool WaitFor(const char* text) {
    const auto deadline = GetTickCount64() + 12000;
    while (GetTickCount64() < deadline && status != TerminalProcess::Status::Failed &&
           status != TerminalProcess::Status::ShellNotFound) {
        if (output.find(text) != std::string::npos) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    }
    return false;
}

bool Check(bool pass, const char* name) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    return pass;
}
}

int main() {
    if (!TerminalProcess::Supported()) {
        std::puts("[SKIP] Windows ConPTY unavailable");
        return 0;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = Receive;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseTerminalTest";
    RegisterClassW(&wc);
    const auto window = CreateWindowExW(0, wc.lpszClassName, nullptr, 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    owner = window;
    if (!window || !terminal.Start(window, L"C:\\", L"PulseTerminalFixture:", {80, 25}, true)) return 1;
    bool passed = Check(WaitFor("PulseTerminalFixture:1"), "PowerShell emits the first prompt");
    terminal.Resize({105, 29});
    passed &= Check(terminal.Write("[Console]::WriteLine('PULSE_' + 'READY:' + (Get-Location).Path)\r") &&
        WaitFor("PULSE_READY:C:\\"), "ConPTY sends input and receives the shell's output from the initial directory");
    passed &= Check(WaitFor("PulseTerminalFixture:2"), "PowerShell reports the next prompt after executing input");
    wchar_t cwd[MAX_PATH]{};
    const auto cwd_length = GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);
    const std::wstring folder = std::wstring(cwd, cwd_length < ARRAYSIZE(cwd) ? cwd_length : 0) +
        L"\\bench_data\\terminal-" + std::to_wstring(GetCurrentProcessId()) + L" '中文’ [fixture]";
    if (cwd_length && cwd_length < ARRAYSIZE(cwd) && CreateDirectoryW(folder.c_str(), nullptr)) {
        const auto command = pulse::ops::TerminalDirectoryInput(folder);
        passed &= Check(!command.empty() && terminal.Write(command) &&
            WaitFor("PulseTerminalFixture:3"), "folder tracking accepts spaces, apostrophes, and Unicode");
        passed &= Check(terminal.Write("[Console]::WriteLine('PULSE_' + 'MATCH:' + ((Get-Location).Path.Contains('[fixture]')))\r") &&
            WaitFor("PULSE_MATCH:True"), "PowerShell treats the destination as a literal filesystem path");
        const auto new_window = CreateWindowExW(0, wc.lpszClassName, nullptr, 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        const auto session = terminal.NotificationId();
        if (new_window) {
            PostMessageW(window, pulse::ops::kTerminalUpdated, session, 0);
            owner = new_window;
            terminal.SetOwner(new_window);
            passed &= Check(terminal.NotificationId() == session &&
                terminal.Write("[Console]::WriteLine('PULSE_' + 'MOVED')\r") &&
                WaitFor("PULSE_MOVED"), "a running ConPTY session delivers output to its new window");
            owner = window;
            terminal.SetOwner(window);
            DestroyWindow(new_window);
        } else {
            passed &= Check(false, "create a destination terminal window");
        }
        passed &= Check(!terminal.HasChildProcesses() &&
            terminal.Write("$null = Start-Job { Start-Sleep -Seconds 8 }; [Console]::WriteLine('PULSE_' + 'JOB')\r") &&
            WaitFor("PULSE_JOB") && terminal.HasChildProcesses(),
            "a PowerShell background job remains detectable after the shell accepts another command");
        terminal.Stop();
        passed &= Check(RemoveDirectoryW(folder.c_str()) != FALSE, "clean up the isolated terminal directory");
    } else {
        std::printf("Cannot create fixture: %lu\n", GetLastError());
        passed &= Check(false, "create isolated terminal directory");
    }
    terminal.Stop();
    output.clear();
    status = TerminalProcess::Status::Starting;
    const bool cmd_started = terminal.Start(window, L"C:\\", L"PulseCmdFixture:", {80, 25}, true,
        TerminalShell::CommandPrompt);
    passed &= Check(cmd_started && WaitFor("PulseCmdFixture:"), "cmd reports its first idle prompt");
    output.clear();
    passed &= Check(cmd_started && terminal.Write("echo PULSE_CMD:%CD%\r") &&
        WaitFor("PULSE_CMD:C:\\") && WaitFor("PulseCmdFixture:"),
        "cmd runs in the current pane directory and reports the next prompt");
    terminal.Stop();

    const auto pwsh = pulse::ops::FindPowerShell7();
    if (!pwsh.empty()) {
        output.clear();
        status = TerminalProcess::Status::Starting;
        passed &= Check(terminal.Start(window, L"C:\\", L"PulsePwshFixture:", {80, 25}, true, TerminalShell::PowerShell7) &&
            WaitFor("PulsePwshFixture:1") && terminal.Write("[Console]::WriteLine('PULSE_' + 'PWSH:' + $PSVersionTable.PSVersion.Major)\r") &&
            WaitFor("PULSE_PWSH:7"), "detected PowerShell 7 runs in the embedded terminal");
        terminal.Stop();
    } else {
        std::puts("[SKIP] PowerShell 7 is absent from the standard path and PATH");
    }
    DestroyWindow(window);
    if (!passed) std::printf("Status %d, output %s\n", static_cast<int>(status), output.c_str());
    return passed ? 0 : 1;
}
