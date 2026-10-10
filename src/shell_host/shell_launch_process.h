#pragma once

#include "../ipc/shell_launch_protocol.h"
#include <functional>
#include <memory>
#include <string>

namespace pulse::shell {

inline std::wstring ShellLaunchPipeName(DWORD broker, uint32_t request) {
    return ipc::PipeNameFor(broker) + L"_launch_" + std::to_wstring(request);
}

// These frames are private to the authenticated broker/worker connection.
constexpr uint32_t kShellLaunchHeartbeat = 0x7fff0101;

class ShellLaunchProcesses {
public:
    using Reply = std::function<void(uint32_t, uint32_t, const std::vector<uint8_t>&)>;
    static constexpr size_t kProcessLimit = 4;
    static constexpr size_t kQueueLimit = 64;
    static constexpr DWORD kNoWindowTimeoutMs = 15000;

    ShellLaunchProcesses(std::wstring executable, Reply reply);
    ~ShellLaunchProcesses();
    ShellLaunchProcesses(const ShellLaunchProcesses&) = delete;
    ShellLaunchProcesses& operator=(const ShellLaunchProcesses&) = delete;
    void SetUiProcess(DWORD process);
    void Submit(uint32_t request, std::vector<uint8_t> payload);
    bool Cancel(uint32_t request);
    void Stop();
    size_t ActiveProcesses() const;

private:
    struct Core;
    std::shared_ptr<Core> core_;
};

int RunShellLaunchWorker(DWORD parent, uint32_t request);
#ifdef PULSE_SHELL_LAUNCH_TESTING
int RunShellLaunchTestCommand(const wchar_t* output);
#endif

} // namespace pulse::shell
