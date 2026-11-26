// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "terminal_shell.h"
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::ops {

inline constexpr UINT kTerminalUpdated = WM_APP + 121;
std::string TerminalDirectoryInput(std::wstring_view directory);
bool IsPowerShell7(const std::wstring& executable);
std::wstring FindPowerShell7();

class TerminalProcess {
public:
    enum class Status { Starting, Running, Exited, Failed, ShellNotFound };
    struct Update {
        std::string output;
        Status status = Status::Starting;
    };

    TerminalProcess() = default;
    ~TerminalProcess();
    TerminalProcess(const TerminalProcess&) = delete;
    TerminalProcess& operator=(const TerminalProcess&) = delete;
    static bool Supported();
    bool Start(HWND window, std::wstring directory, std::wstring prompt_marker, COORD size, bool isolated = false,
               TerminalShell shell = TerminalShell::WindowsPowerShell, std::wstring pwsh_path = {});
    void Stop();
    void SetOwner(HWND window);
    UINT_PTR NotificationId() const;
    Update TakeOutput();
    bool Write(std::string_view bytes);
    void Resize(COORD size);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::ops
