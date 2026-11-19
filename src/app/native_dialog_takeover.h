// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#pragma once

#include <windows.h>
#include <d2d1.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulse::app {

class NativeDialogTakeover {
public:
    NativeDialogTakeover() = default;
    ~NativeDialogTakeover();
    NativeDialogTakeover(const NativeDialogTakeover&) = delete;
    NativeDialogTakeover& operator=(const NativeDialogTakeover&) = delete;
    bool Configure(bool enabled, bool dark, D2D1_COLOR_F accent, bool show_hidden,
                   std::wstring pulse_folder = {}, bool verify_copies = false);
    void Stop();
    // The integration fixture watches only its own child process.
    bool StartForTesting(DWORD source_process);
    bool ReadyForTesting() const;
    bool StoppedForTesting() const;
    bool ObserveForTesting(HWND source);
    struct Shared;

private:
    bool ConfigureImpl(bool enabled, bool dark, D2D1_COLOR_F accent, bool show_hidden,
                       std::wstring pulse_folder, DWORD source_process, bool verify_copies);
    mutable std::mutex mutex_;
    std::shared_ptr<Shared> shared_;
    std::thread watcher_;
    bool stopped_ = false;
};

} // namespace pulse::app
