#pragma once
#include "window_result_mailbox.h"
#include <memory>

namespace pulse::app {

struct ShellRegistryWatchContext {
    ShellRegistryWatchContext() = default;
    ShellRegistryWatchContext(const ShellRegistryWatchContext&) = delete;
    ShellRegistryWatchContext& operator=(const ShellRegistryWatchContext&) = delete;
    std::shared_ptr<WindowResultMailbox<bool>> notifications;
    HANDLE stop = nullptr;

    bool DuplicateStop(HANDLE source) {
        return DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
            &stop, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
    }
    ~ShellRegistryWatchContext() { if (stop) CloseHandle(stop); }
};

inline void StopShellRegistryWatchHandles(HANDLE& stop, HANDLE& thread, DWORD timeout = 2000) {
    if (stop) SetEvent(stop);
    if (thread) {
        WaitForSingleObject(thread, timeout);
        CloseHandle(thread);
        thread = nullptr;
    }
    if (stop) {
        CloseHandle(stop);
        stop = nullptr;
    }
}

} // namespace pulse::app
