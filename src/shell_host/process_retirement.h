// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <array>
#include <cassert>
#include <mutex>

namespace pulse::shell {
namespace retirement_detail {
#ifdef PULSE_SHELL_RETIREMENT_TESTING
DWORD WaitForExit(HANDLE process, DWORD timeout);
BOOL TerminateWorker(HANDLE process);
#else
inline DWORD WaitForExit(HANDLE process, DWORD timeout) {
    return WaitForSingleObject(process, timeout);
}
inline BOOL TerminateWorker(HANDLE process) {
    return TerminateProcess(process, ERROR_CANCELLED);
}
#endif
}

template<size_t Capacity>
class ProcessRetirements {
public:
    ~ProcessRetirements() {
        for (HANDLE process : processes_) if (process) CloseHandle(process);
    }

    bool Retire(HANDLE& process) noexcept {
        if (!process || process == INVALID_HANDLE_VALUE) return true;
        if (retirement_detail::WaitForExit(process, 0) != WAIT_OBJECT_0) {
            retirement_detail::TerminateWorker(process);
            // Termination can remain pending on provider/driver I/O. Preserve
            // its admission slot without keeping a runner or shutdown waiting.
            if (retirement_detail::WaitForExit(process, 1000) != WAIT_OBJECT_0) {
                std::lock_guard lock(mutex_);
                for (auto& retired : processes_) {
                    if (retired) continue;
                    retired = process;
                    process = nullptr;
                    return false;
                }
                assert(false); // Owners admit at most Capacity live processes.
            }
        }
        CloseHandle(process);
        process = nullptr;
        return true;
    }

    size_t Reap() noexcept {
        std::lock_guard lock(mutex_);
        size_t reaped = 0;
        for (auto& process : processes_) {
            if (!process) continue;
            if (retirement_detail::WaitForExit(process, 0) == WAIT_OBJECT_0) {
                CloseHandle(process);
                process = nullptr;
                ++reaped;
            }
        }
        return reaped;
    }

private:
    std::mutex mutex_;
    std::array<HANDLE, Capacity> processes_{};
};

} // namespace pulse::shell
